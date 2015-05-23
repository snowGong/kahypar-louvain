/*
 * BFSInitialPartitioning.h
 *
 *  Created on: Apr 12, 2015
 *      Author: theuer
 */

#ifndef SRC_PARTITION_INITIAL_PARTITIONING_BFSINITIALPARTITIONER_H_
#define SRC_PARTITION_INITIAL_PARTITIONING_BFSINITIALPARTITIONER_H_

#include <queue>
#include <unordered_map>
#include <algorithm>

#include "lib/definitions.h"
#include "partition/initial_partitioning/IInitialPartitioner.h"
#include "partition/initial_partitioning/InitialPartitionerBase.h"
#include "partition/initial_partitioning/policies/StartNodeSelectionPolicy.h"
#include "tools/RandomFunctions.h"

using defs::HypernodeWeight;
using partition::StartNodeSelectionPolicy;

namespace partition {

template<class StartNodeSelection = StartNodeSelectionPolicy>
class BFSInitialPartitioner: public IInitialPartitioner,
		private InitialPartitionerBase {

public:
	BFSInitialPartitioner(Hypergraph& hypergraph, Configuration& config) :
			InitialPartitionerBase(hypergraph, config) {
	}

	~BFSInitialPartitioner() {
	}

private:
	FRIEND_TEST(ABFSBisectionInitialPartionerTest, HasCorrectInQueueMapValuesAfterPushingIncidentHypernodesNodesIntoQueue);
	FRIEND_TEST(ABFSBisectionInitialPartionerTest, HasCorrectHypernodesIntoQueueAfterPushingIncidentHypernodesIntoQueue);

	// TODO(heuer): Rename this method. Current method name is misleading!
	// TODO(heuer): Make all parameters that should not be modified const: In this case unassigned_part
	void pushIncidentHyperedgesIntoQueue(std::queue<HypernodeID>& q,
			HypernodeID hn, std::unordered_map<HypernodeID, bool>& in_queue,
			PartitionID& unassigned_part) {
		// TODO(heuer): Make he und hnodes const
		for (HyperedgeID he : _hg.incidentEdges(hn)) {
			for (HypernodeID hnodes : _hg.pins(he)) {
				if (_hg.partID(hnodes) == unassigned_part
						&& !in_queue[hnodes]) {
					q.push(hnodes);
					in_queue[hnodes] = true;
				}
			}
		}
	}

	// TODO(heuer): If I'm right, the k-way impl could be implemented in the same way as the different
	// greedy variants. Did you try these as well?
	void kwayPartitionImpl() final {
		PartitionID unassigned_part =
				_config.initial_partitioning.unassigned_part;
		InitialPartitionerBase::resetPartitioning(unassigned_part);

		// TODO(heuer): I think it would be better to make this a member of BFSInitialPartitioner.
		// In case of multiple runs etc. there is no need to always create these data structures
		// and a simple reset/clear would be enough.
		//Initialize a vector of queues for each partition
		std::vector<std::queue<HypernodeID>> q(_config.initial_partitioning.k,
				std::queue<HypernodeID>());

		//Initialize a vector for each partition, which indicate if a partition is ready to receive further hypernodes.
		std::vector<bool> partEnable(_config.initial_partitioning.k, true);
		if (unassigned_part != -1) {
			partEnable[unassigned_part] = false;
		}

		HypernodeWeight assigned_nodes_weight = 0;
		if (unassigned_part != -1) {
			assigned_nodes_weight =
					_config.initial_partitioning.perfect_balance_partition_weight[unassigned_part]
							* (1.0 - _config.initial_partitioning.epsilon);
		}

		//Initialize a vector for each partition, which indicates the hypernodes which are and were already in the queue.
		//TODO(heuer): This can be done more efficiently. Why not use vector<bool> instead of unordered maps?
		std::vector<std::unordered_map<HypernodeID, bool>> in_queue(_hg.k());

		//Calculate Startnodes and push them into the queues.
		std::vector<HypernodeID> startNodes;
		StartNodeSelection::calculateStartNodes(startNodes, _hg,
				_config.initial_partitioning.k);
		// TODO(heuer): This loop should loop over k instead of i for more clarity.
		// TODO(heuer): Also, why build the start node vector only to then insert the nodes into
		// the queue. Why not directly insert them into the queue?
		for (unsigned int i = 0; i < startNodes.size(); i++) {
			q[i].push(startNodes[i]);
			in_queue[i][startNodes[i]] = true;
		}

		// TODO(heuer): When you ennumerate hypernodes, please use HypernodeID as type.
		while (assigned_nodes_weight < _config.partition.total_graph_weight) {
			bool every_part_is_disable = true;
			for (PartitionID i = 0; i < _config.initial_partitioning.k; i++) {
				every_part_is_disable = every_part_is_disable && !partEnable[i];
				if (partEnable[i]) {
					// TODO(heuer): Better use an invalid value for initialization, since 0 is a valid hypernode.
					// If queues are empty, you might end up with hn0 after the search, although actually
					// you did not find a unassigned node.
					HypernodeID hn = 0;

					//Searching for an unassigned hypernode in queue for Partition i
					if (!q[i].empty()) {
						hn = q[i].front();
						q[i].pop();
						while (_hg.partID(hn) != unassigned_part
								&& !q[i].empty()) {
							hn = q[i].front();
							q[i].pop();
						}
					}

					//If no unassigned hypernode was found we have to select a new startnode.
					if (_hg.partID(hn) != unassigned_part && q[i].empty()) {
						hn = InitialPartitionerBase::getUnassignedNode(
								unassigned_part);
						in_queue[i][hn] = true;
					}

					ASSERT(_hg.partID(hn) == unassigned_part,
							"Hypernode " << hn << " isn't a node from an unassigned part.");

					pushIncidentHyperedgesIntoQueue(q[i], hn, in_queue[i],
							unassigned_part);

					ASSERT(
							[&]() { for (HyperedgeID he : _hg.incidentEdges(hn)) { for (HypernodeID hnodes : _hg.pins(he)) { if (_hg.partID(hnodes) == unassigned_part && !in_queue[i][hnodes]) { return false; } } } return true; }(),
							"Some hypernodes are missing into the queue!");

					if (assignHypernodeToPartition(hn, i)) {
						assigned_nodes_weight += _hg.nodeWeight(hn);
					} else {
						if (q[i].empty()) {
							partEnable[i] = false;
						}
					}
				}
			}

			if (every_part_is_disable) {
				break;
			}
		}
		InitialPartitionerBase::rollbackToBestCut();
		InitialPartitionerBase::performFMRefinement();
	}

	void bisectionPartitionImpl() final {
		PartitionID k = _config.initial_partitioning.k;
		_config.initial_partitioning.k = 2;
		kwayPartitionImpl();
		_config.initial_partitioning.k = k;
	}

	// TODO(heuer): What about the implementation that grows from both sides?
	/*void bisectionPartitionImpl() final {
	 PartitionID unassigned_part = 1;
	 std::queue<HypernodeID> bfs;
	 // TODO(heuer): Again, vector<bool> should be faster
	 std::unordered_map<HypernodeID, bool> in_queue;

	 //Calculate Startnode and push them into the queues.
	 std::vector<HypernodeID> startNode;
	 StartNodeSelection::calculateStartNodes(startNode, _hg,
	 static_cast<PartitionID>(2));
	 bfs.push(startNode[0]);
	 InitialPartitionerBase::resetPartitioning(unassigned_part);
	 in_queue[startNode[0]] = true;

	 // TODO(heuer): Why outside the loop? Can be moved into the loop and made const.
	 // Compiler should be smart enough to realize that it will need the variable in
	 // every iteration.
	 HypernodeID hn;
	 do {

	 //Searching for an unassigned hypernode in the queue.
	 if (!bfs.empty()) {
	 hn = bfs.front();
	 bfs.pop();
	 }
	 //If no unassigned hypernode was found we have to select a new startnode.
	 else {
	 // TODO(heuer): I would expect this method to be part of the StartNodeSelection Policy
	 hn = InitialPartitionerBase::getUnassignedNode(unassigned_part);
	 in_queue[hn] = true;
	 }

	 ASSERT(_hg.partID(hn) == unassigned_part,
	 "Hypernode " << hn << " isn't a node from an unassigned part.");

	 pushIncidentHyperedgesIntoQueue(bfs, hn, in_queue, unassigned_part);
	 } while (assignHypernodeToPartition(hn, 0));

	 InitialPartitionerBase::rollbackToBestCut();
	 InitialPartitionerBase::performFMRefinement();
	 }*/

	using InitialPartitionerBase::_hg;
	using InitialPartitionerBase::_config;

};

}

#endif /* SRC_PARTITION_INITIAL_PARTITIONING_BFSINITIALPARTITIONER_H_ */