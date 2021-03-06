/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/check_quorum_for_config_change.h"

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/db/repl/repl_set_heartbeat_args.h"
#include "mongo/db/repl/replica_set_config.h"
#include "mongo/db/repl/scatter_gather_algorithm.h"
#include "mongo/db/repl/scatter_gather_runner.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace repl {
namespace {
    /**
     * Quorum checking state machine.
     *
     * Usage: Construct a QuorumChecker, pass in a pointer to the configuration for which you're
     * checking quorum, and the integer index of the member config representing the "executing"
     * node.  Use ScatterGatherRunner or otherwise execute a scatter-gather procedure as desribed in
     * the class comment for the ScatterGatherAlgorithm class.  After
     * hasReceivedSufficientResponses() returns true, you may call getFinalStatus() to get the
     * result of the quorum check.
     */
    class QuorumChecker : public ScatterGatherAlgorithm {
        MONGO_DISALLOW_COPYING(QuorumChecker);
    public:
        /**
         * Constructs a QuorumChecker that is used to confirm that sufficient nodes are up to accept
         * "rsConfig".  "myIndex" is the index of the local node, which is assumed to be up.
         *
         * "rsConfig" must stay in scope until QuorumChecker's destructor completes.
         */
        QuorumChecker(const ReplicaSetConfig* rsConfig, int myIndex);
        virtual ~QuorumChecker();

        virtual std::vector<ReplicationExecutor::RemoteCommandRequest> getRequests() const;
        virtual void processResponse(
                const ReplicationExecutor::RemoteCommandRequest& request,
                const ResponseStatus& response);

        virtual bool hasReceivedSufficientResponses() const;

        Status getFinalStatus() const { return _finalStatus; }

    private:
        /**
         * Callback that executes after _haveReceivedSufficientReplies() becomes true.
         *
         * Computes the quorum result based on responses received so far, stores it into
         * _finalStatus, and enables QuorumChecker::run() to return.
         */
        void _onQuorumCheckComplete();

        /**
         * Updates the QuorumChecker state based on the data from a single heartbeat response.
         */
        void _tabulateHeartbeatResponse(
                const ReplicationExecutor::RemoteCommandRequest& request,
                const ResponseStatus& response);

        // Pointer to the replica set configuration for which we're checking quorum.
        const ReplicaSetConfig* const _rsConfig;

        // Index of the local node's member configuration in _rsConfig.
        const int _myIndex;

        // List of nodes believed to be down.
        std::vector<HostAndPort> _down;

        // List of voting nodes that have responded affirmatively.
        std::vector<HostAndPort> _voters;

        // Total number of responses and timeouts processed.
        int _numResponses;

        // Number of electable nodes that have responded affirmatively.
        int _numElectable;

        // Set to a non-OK status if a response from a remote node indicates
        // that the quorum check should definitely fail, such as because of
        // a replica set name mismatch.
        Status _vetoStatus;

        // Final status of the quorum check, returned by run().
        Status _finalStatus;
    };

    QuorumChecker::QuorumChecker(const ReplicaSetConfig* rsConfig, int myIndex)
        : _rsConfig(rsConfig),
          _myIndex(myIndex),
          _numResponses(1),  // We "responded" to ourself already.
          _numElectable(0),
          _vetoStatus(Status::OK()),
          _finalStatus(ErrorCodes::CallbackCanceled, "Quorum check canceled") {

        invariant(myIndex < _rsConfig->getNumMembers());
        const MemberConfig& myConfig = _rsConfig->getMemberAt(_myIndex);

        if (myConfig.isVoter()) {
            _voters.push_back(myConfig.getHostAndPort());
        }
        if (myConfig.isElectable()) {
            _numElectable = 1;
        }

        if (hasReceivedSufficientResponses()) {
            _onQuorumCheckComplete();
        }
    }

    QuorumChecker::~QuorumChecker() {}

    std::vector<ReplicationExecutor::RemoteCommandRequest> QuorumChecker::getRequests() const {
        const bool isInitialConfig = _rsConfig->getConfigVersion() == 1;
        const MemberConfig& myConfig = _rsConfig->getMemberAt(_myIndex);

        std::vector<ReplicationExecutor::RemoteCommandRequest> requests;
        if (hasReceivedSufficientResponses()) {
            return requests;
        }

        ReplSetHeartbeatArgs hbArgs;
        hbArgs.setSetName(_rsConfig->getReplSetName());
        hbArgs.setProtocolVersion(1);
        hbArgs.setConfigVersion(_rsConfig->getConfigVersion());
        hbArgs.setCheckEmpty(isInitialConfig);
        hbArgs.setSenderHost(myConfig.getHostAndPort());
        hbArgs.setSenderId(myConfig.getId());
        const BSONObj hbRequest = hbArgs.toBSON();

        // Send a bunch of heartbeat requests.
        // Schedule an operation when a "sufficient" number of them have completed, and use that
        // to compute the quorum check results.
        // Wait for the "completion" callback to finish, and then it's OK to return the results.
        for (int i = 0; i < _rsConfig->getNumMembers(); ++i) {
            if (_myIndex == i) {
                // No need to check self for liveness or unreadiness.
                continue;
            }
            requests.push_back(ReplicationExecutor::RemoteCommandRequest(
                                       _rsConfig->getMemberAt(i).getHostAndPort(),
                                       "admin",
                                       hbRequest,
                                       _rsConfig->getHeartbeatTimeoutPeriodMillis()));
        }

        return requests;
    }

    void QuorumChecker::processResponse(
            const ReplicationExecutor::RemoteCommandRequest& request,
            const ResponseStatus& response) {

        _tabulateHeartbeatResponse(request, response);
        if (hasReceivedSufficientResponses()) {
            _onQuorumCheckComplete();
        }
    }

    void QuorumChecker::_onQuorumCheckComplete() {
        if (!_vetoStatus.isOK()) {
            _finalStatus = _vetoStatus;
            return;
        }
        if (_rsConfig->getConfigVersion() == 1 && !_down.empty()) {
            str::stream message;
            message << "Could not contact the following nodes during replica set initiation: " <<
                _down.front().toString();
            for (size_t i = 1; i < _down.size(); ++i) {
                message << ", " << _down[i].toString();
            }
            _finalStatus = Status(ErrorCodes::NodeNotFound, message);
            return;
        }
        if (_numElectable == 0) {
            _finalStatus = Status(
                    ErrorCodes::NodeNotFound, "Quorum check failed because no "
                    "electable nodes responded; at least one required for config");
            return;
        }
        if (int(_voters.size()) < _rsConfig->getMajorityVoteCount()) {
            str::stream message;
            message << "Quorum check failed because not enough voting nodes responded; required " <<
                _rsConfig->getMajorityVoteCount() << " but ";

            if (_voters.size() == 0) {
                message << "none responded";
            }
            else {
                message << "only the following " << _voters.size() <<
                    " voting nodes responded: " << _voters.front().toString();
                for (size_t i = 1; i < _voters.size(); ++i) {
                    message << ", " << _voters[i].toString();
                }
            }
            _finalStatus = Status(ErrorCodes::NodeNotFound, message);
            return;
        }
        _finalStatus = Status::OK();
    }

    void QuorumChecker::_tabulateHeartbeatResponse(
            const ReplicationExecutor::RemoteCommandRequest& request,
            const ResponseStatus& response) {

        ++_numResponses;
        if (!response.isOK()) {
            warning() << "Failed to complete heartbeat request to " << request.target <<
                "; " << response.getStatus();
            _down.push_back(request.target);
            return;
        }
        BSONObj res = response.getValue().data;
        if (res["mismatch"].trueValue()) {
            std::string message = str::stream() << "Our set name did not match that of " <<
                request.target.toString();
            _vetoStatus = Status(ErrorCodes::NewReplicaSetConfigurationIncompatible, message);
            warning() << message;
            return;
        }
        if (res.getStringField("set")[0] != '\0') {
            if (res["v"].numberInt() >= _rsConfig->getConfigVersion()) {
                std::string message = str::stream() << "Our config version of " <<
                    _rsConfig->getConfigVersion() <<
                    " is no larger than the version on " << request.target.toString() <<
                    ", which is " << res["v"].toString();
                _vetoStatus = Status(ErrorCodes::NewReplicaSetConfigurationIncompatible, message);
                warning() << message;
                return;
            }
        }
        if (!res["ok"].trueValue()) {
            warning() << "Got error response on heartbeat request to " << request.target <<
                "; " << res;
            _down.push_back(request.target);
            return;
        }

        for (int i = 0; i < _rsConfig->getNumMembers(); ++i) {
        const MemberConfig& memberConfig = _rsConfig->getMemberAt(i);
            if (memberConfig.getHostAndPort() != request.target) {
                continue;
            }
            if (memberConfig.isElectable()) {
                ++_numElectable;
            }
            if (memberConfig.isVoter()) {
                _voters.push_back(request.target);
            }
            return;
        }
        invariant(false);
    }

    bool QuorumChecker::hasReceivedSufficientResponses() const {
        if (!_vetoStatus.isOK() || _numResponses == _rsConfig->getNumMembers()) {
            // Vetoed or everybody has responded.  All done.
            return true;
        }
        if (_rsConfig->getConfigVersion() == 1) {
            // Have not received responses from every member, and the proposed config
            // version is 1 (initial configuration).  Keep waiting.
            return false;
        }
        if (_numElectable == 0) {
            // Have not heard from at least one electable node.  Keep waiting.
            return false;
        }
        if (int(_voters.size()) < _rsConfig->getMajorityVoteCount()) {
            // Have not heard from a majority of voters.  Keep waiting.
            return false;
        }

        // Have heard from a majority of voters and one electable node.  All done.
        return true;
    }

    Status checkQuorumGeneral(ReplicationExecutor* executor,
                              const ReplicaSetConfig& rsConfig,
                              const int myIndex) {
        QuorumChecker checker(&rsConfig, myIndex);
        ScatterGatherRunner runner(&checker);
        Status status = runner.run(executor);
        if (!status.isOK()) {
            return status;
        }

        return checker.getFinalStatus();
    }
}  // namespace

    Status checkQuorumForInitiate(ReplicationExecutor* executor,
                                  const ReplicaSetConfig& rsConfig,
                                  const int myIndex) {
        invariant(rsConfig.getConfigVersion() == 1);
        return checkQuorumGeneral(executor, rsConfig, myIndex);
    }

    Status checkQuorumForReconfig(ReplicationExecutor* executor,
                                  const ReplicaSetConfig& rsConfig,
                                  const int myIndex) {
        invariant(rsConfig.getConfigVersion() > 1);
        return checkQuorumGeneral(executor, rsConfig, myIndex);
    }

}  // namespace repl
}  // namespace mongo
