/*
 * LabelPropagationInitialPartitioner.h
 *
 *  Created on: May 8, 2015
 *      Author: theuer
 */

#ifndef SRC_PARTITION_INITIAL_PARTITIONING_LABELPROPAGATIONINITIALPARTITIONER_H_
#define SRC_PARTITION_INITIAL_PARTITIONING_LABELPROPAGATIONINITIALPARTITIONER_H_

#define MAX_ITERATIONS 100

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <utility>
#include <vector>

#include "lib/datastructure/FastResetBitVector.h"
#include "lib/definitions.h"
#include "partition/initial_partitioning/IInitialPartitioner.h"
#include "partition/initial_partitioning/InitialPartitionerBase.h"
#include "partition/initial_partitioning/policies/GainComputationPolicy.h"
#include "partition/initial_partitioning/policies/StartNodeSelectionPolicy.h"
#include "tools/RandomFunctions.h"

using defs::Hypergraph;
using defs::HypernodeWeight;
using defs::HypernodeID;
using defs::HyperedgeID;
using partition::GainComputationPolicy;
using partition::StartNodeSelectionPolicy;
using Gain = HyperedgeWeight;

namespace partition {
template <class StartNodeSelection = StartNodeSelectionPolicy,
          class GainComputation = GainComputationPolicy>
class LabelPropagationInitialPartitioner : public IInitialPartitioner,
                                           private InitialPartitionerBase {
 public:
  LabelPropagationInitialPartitioner(Hypergraph& hypergraph,
                                     Configuration& config) :
    InitialPartitionerBase(hypergraph, config),
    _valid_parts(config.initial_partitioning.k, false),
    _tmp_scores(_config.initial_partitioning.k, 0) { }

  ~LabelPropagationInitialPartitioner() { }

  void kwayPartitionImpl() final {
    PartitionID unassigned_part =
      _config.initial_partitioning.unassigned_part;
    _config.initial_partitioning.unassigned_part = -1;
    InitialPartitionerBase::resetPartitioning();

    std::vector<HypernodeID> nodes(_hg.numNodes(), 0);
    for (HypernodeID hn : _hg.nodes()) {
      nodes[hn] = hn;
    }

    int assigned_nodes = 0;
    int lambda = 1;
    int connected_nodes = 5;
    std::vector<HypernodeID> startNodes;
    StartNodeSelection::calculateStartNodes(startNodes, _hg,
                                            lambda * _config.initial_partitioning.k);
    for (PartitionID i = 0; i < _config.initial_partitioning.k; i++) {
      assigned_nodes += bfsAssignHypernodeToPartition(startNodes[i], i,
                                                      connected_nodes);
    }

    ASSERT(
      [&]() {
        for (PartitionID i = 0; i < _config.initial_partitioning.k; i++) {
          if (_hg.partSize(i) != connected_nodes) {
            return false;
          }
        }
        return true;
      } (),
      "Size of a partition is not equal " << connected_nodes << "!");

    bool converged = false;
    int iterations = 0;
    int changes = 0;
    while (!converged && iterations < MAX_ITERATIONS) {
      converged = true;
      changes = 0;

      // TODO(heuer): is this shuffle really necessary in EACH iteration?
      // This call is the TOP hotstop in LP refiner!
      Randomize::shuffleVector(nodes, nodes.size());
      for (HypernodeID v : nodes) {
        std::pair<PartitionID, Gain> max_move = computeMaxGainMove(v);
        PartitionID max_part = max_move.first;
        Gain max_gain = max_move.second;

        if (max_part != _hg.partID(v)) {
          PartitionID source_part = _hg.partID(v);

          ASSERT(
            [&]() {
              for (HyperedgeID he : _hg.incidentEdges(v)) {
                for (PartitionID part : _hg.connectivitySet(he)) {
                  if (part == max_part) {
                    return true;
                  }
                }
              }
              return false;
            } (),
            "Partition " << max_part << " is not an incident label of hypernode " << v << "!");

          if (InitialPartitionerBase::assignHypernodeToPartition(v,
                                                                 max_part)) {
            ASSERT(
              [&]() {
                if (source_part != -1) {
                  Gain gain = max_gain;
                  _hg.changeNodePart(v, max_part, source_part);
                  HyperedgeWeight cut_before = metrics::hyperedgeCut(_hg);
                  _hg.changeNodePart(v, source_part, max_part);
                  return metrics::hyperedgeCut(_hg) == (cut_before - gain);
                }
                return true;
              } (),
              "Gain calculation failed of hypernode " << v << " failed from part " << source_part << " to " << max_part << "!");
            changes++;
            if (source_part == -1) {
              assigned_nodes++;
            }
            converged = false;
          }
        }
      }
      iterations++;

      int assign_unassigned_node_count = 5;
      if (converged && assigned_nodes < _hg.numNodes()) {
        for (HypernodeID hn : nodes) {
          if (_hg.partID(hn) == -1) {
            assignHypernodeToPartitionWithMinimumPartitionWeight(
              hn);
            converged = false;
            assigned_nodes++;
            assign_unassigned_node_count--;
            if (!assign_unassigned_node_count) {
              break;
            }
          }
        }
      }
    }

    for (HypernodeID hn : _hg.nodes()) {
      if (_hg.partID(hn) == -1) {
        assignHypernodeToPartitionWithMinimumPartitionWeight(hn);
      }
    }

    _config.initial_partitioning.unassigned_part = unassigned_part;

    ASSERT([&]() {
        for (HypernodeID hn : _hg.nodes()) {
          if (_hg.partID(hn) == -1) {
            return false;
          }
        }
        return true;
      } (), "There are unassigned hypernodes!");

    _hg.initializeNumCutHyperedges();
    InitialPartitionerBase::performFMRefinement();
  }

  void bisectionPartitionImpl() final {
    PartitionID k = _config.initial_partitioning.k;
    _config.initial_partitioning.k = 2;
    kwayPartitionImpl();
    _config.initial_partitioning.k = k;
  }

 private:
  FRIEND_TEST(ALabelPropagationMaxGainMoveTest, AllMaxGainMovesAZeroGainMovesIfNoHypernodeIsAssigned);
  FRIEND_TEST(ALabelPropagationMaxGainMoveTest, MaxGainMoveIsAZeroGainMoveIfANetHasOnlyOneAssignedHypernode);
  FRIEND_TEST(ALabelPropagationMaxGainMoveTest, CalculateMaxGainMoveIfThereAStillUnassignedNodesOfAHyperedgeAreLeft);
  FRIEND_TEST(ALabelPropagationMaxGainMoveTest, CalculateMaxGainMoveOfAnAssignedHypernodeIfAllHypernodesAreAssigned);
  FRIEND_TEST(ALabelPropagationMaxGainMoveTest, CalculateMaxGainMoveOfAnUnassignedHypernodeIfAllHypernodesAreAssigned);

  std::pair<PartitionID, Gain> computeMaxGainMove(const HypernodeID& hn) {
    ASSERT(std::all_of(_tmp_scores.begin(), _tmp_scores.end(), [](Gain i) { return i == 0; }),
           "Temp gain array not initialized properly");
    _valid_parts.resetAllBitsToFalse();

    PartitionID source_part = _hg.partID(hn);
    HyperedgeWeight internal_weight = 0;
    for (const HyperedgeID he : _hg.incidentEdges(hn)) {
      const HypernodeID pins_in_source_part =
        (source_part == -1) ?
        2 : _hg.pinCountInPart(he, source_part);
      if (_hg.connectivity(he) == 1 && pins_in_source_part > 1) {
        PartitionID part_in_edge =
          std::numeric_limits<PartitionID>::min();
        for (const PartitionID part : _hg.connectivitySet(he)) {
          _valid_parts.setBit(part, true);
          part_in_edge = part;
        }
        internal_weight += _hg.edgeWeight(he);
        _tmp_scores[part_in_edge] += _hg.edgeWeight(he);
      } else {
        for (const PartitionID target_part : _hg.connectivitySet(he)) {
          _valid_parts.setBit(target_part, true);
          if (_hg.connectivity(he) == 2 && source_part != -1 &&
              source_part != target_part) {
            const HypernodeID pins_in_target_part =
              _hg.pinCountInPart(he, target_part);
            if (pins_in_source_part == 1) {
              _tmp_scores[target_part] += _hg.edgeWeight(he);
            }
          }
        }
      }
    }

    PartitionID max_part = _hg.partID(hn);
    double max_score = (_hg.partID(hn) == -1) ? invalid_score_value : 0;
    for (PartitionID p = 0; p < _config.initial_partitioning.k; ++p) {
      if (_valid_parts[p]) {
        _tmp_scores[p] -= internal_weight;

        /**ASSERT([&]() {
           Gain gain = GainComputation::calculateGain(_hg,hn,p);
           if(tmp_scores[p] != gain) {
           return false;
           }
           return true;
           }(), "Calculated gain of hypernode " << hn << " is not " << tmp_scores[p] << ".");*/

        const HypernodeWeight new_partition_weight = _hg.partWeight(p) + _hg.nodeWeight(hn);
        if (new_partition_weight
            <= _config.initial_partitioning.upper_allowed_partition_weight[p]) {
          if (_tmp_scores[p] > max_score) {
            max_score = _tmp_scores[p];
            max_part = p;
          }
        }
        _tmp_scores[p] = 0;
      }
    }

    return std::make_pair(max_part, static_cast<Gain>(max_score));
  }

  void assignAllUnassignedNodes(bool& converged) {
    for (HypernodeID hn : _hg.nodes()) {
      if (_hg.partID(hn) == -1) {
        converged = false;
        bfsAssignHypernodeToPartition(hn,
                                      Randomize::getRandomInt(0,
                                                              _config.initial_partitioning.k - 1));
      }
    }
  }

  void assignHypernodeToPartitionWithMinimumPartitionWeight(HypernodeID hn) {
    ASSERT(_hg.partID(hn) == -1,
           "Hypernode " << hn << " isn't part from partition -1!");
    PartitionID p = -1;
    HypernodeWeight min_partition_weight = std::numeric_limits<
      HypernodeWeight>::max();
    for (PartitionID i = 0; i < _config.initial_partitioning.k; i++) {
      if (_hg.partWeight(i) < min_partition_weight) {
        min_partition_weight = _hg.partWeight(i);
        p = i;
      }
    }
    _hg.setNodePart(hn, p);
  }

  int bfsAssignHypernodeToPartition(HypernodeID hn, PartitionID p, int count =
                                      std::numeric_limits<int>::max()) {
    // TODO(heuer): reuse these datastructures... this code will be called MANY times....
    std::queue<HypernodeID> q;
    // TODO(heuer): REMOVE map and use FastResetBitVector instead
    std::map<HypernodeID, bool> in_queue;
    int assigned_nodes = 0;
    q.push(hn);
    in_queue[hn] = true;
    while (!q.empty()) {
      HypernodeID node = q.front();
      q.pop();
      if (_hg.partID(node) == -1) {
        _hg.setNodePart(node, p);
        assigned_nodes++;
        for (HyperedgeID he : _hg.incidentEdges(node)) {
          for (HypernodeID pin : _hg.pins(he)) {
            if (_hg.partID(pin) == -1 && !in_queue[pin]) {
              q.push(pin);
              in_queue[pin] = true;
            }
          }
        }
      }
      if (assigned_nodes == count) {
        break;
      } else if (q.empty()) {
        q.push(InitialPartitionerBase::getUnassignedNode());
      }
    }
    return assigned_nodes;
  }

  double score1(HyperedgeID he) {
    return static_cast<double>(_hg.edgeWeight(he)) / (_hg.edgeSize(he) - 1);
  }

  double score2(HyperedgeID he) {
    return static_cast<double>(_hg.edgeWeight(he)) / (_hg.connectivity(he));
  }

 protected:
  using InitialPartitionerBase::_hg;
  using InitialPartitionerBase::_config;
  FastResetBitVector<> _valid_parts;
  std::vector<Gain> _tmp_scores;

  const double invalid_score_value = std::numeric_limits<double>::lowest();
};
}  // namespace partition

#endif  // SRC_PARTITION_INITIAL_PARTITIONING_LABELPROPAGATIONINITIALPARTITIONER_H_