#include "torch/csrc/jit/passes/graph_fuser.h"
#include <unordered_map>

namespace torch { namespace jit {

namespace {

// What is a simple mappable operator?  It is:
//    - Has an output with the same types and sizes of its input
//    - Single output
//    - Can handle non-contiguous input
//    - Produces contiguous output
// Some of these restrictions may be relaxable, but you should
// carefully read the code first, as we rely on these assumptions.
std::unordered_set<NodeKind> simple_mappable = {
  k__and__,
  k__lshift__,
  k__or__,
  k__rshift__,
  k__xor__,
  kabs,
  kacos,
  kadd,
  kasin,
  katan,
  katan2,
  kceil,
  kclamp,
  kcos,
  kcosh,
  kdiv,
  keq,
  kexp,
  kfloor,
  kfmod,
  kfrac,
  kge,
  kgt,
  kle,
  klerp,
  klgamma,
  klog,
  klog1p,
  klt,
  kmax,
  kmin,
  kmul,
  kne,
  kneg,
  kones,
  kpow,
  kreciprocal,
  kremainder,
  kround,
  krsqrt,
  ksigmoid,
  ksin,
  ksinh,
  ksqrt,
  ksub,
  ktan,
  ktanh,
  ktrunc,
  kzeros,
  "_sigmoid_backward"_sym,
  "_tanh_backward"_sym,
};

bool isSimpleMap(Node *node) {
  if(simple_mappable.count(node->kind())) {
    if(node->kind() == kmin || node->kind() == kmax)
      return node->inputs().size() == 2; // unary min/max is a reduction...
    return true;
  }
  return false;
}

struct GraphFuser {
  std::shared_ptr<Graph>& graph;

  // Used to order nodes so we always consider producer-consumer fusions
  // in reverse topological order.
  // If topological_index[a] > topological_index[b] then a occurs after b.
  // Because nodes can be added to this graph during optimization, this mapping is not bijective.
  // Newly generated nodes will copy the location where they are inserted.
  std::unordered_map<Node*,size_t> topological_index;

  GraphFuser(std::shared_ptr<Graph>& graph)
  : graph(graph) {}

  bool isCuda(Node * node) {
    return node->output()->type()->expect<TensorType>()->device() != -1;
  }
  // TODO: the fusion compiler has a lot of float-specific codegen
  // so for now we only consider nodes that operate on floating point numbers
  bool hasFloatType(Value * node) {
    if(!node->hasType()) {
      return false;
    }
    if(auto tt = node->type()->cast<TensorType>()) {
      return tt->scalarType() == at::kFloat;
    } else {
      return false;
    }
  }
  bool allFloatIO(Node * node) {
    for(auto & o : node->outputs()) {
      if(!hasFloatType(o)) {
        return false;
      }
    }
    for(auto & o : node->inputs()) {
      if(!hasFloatType(o)) {
        return false;
      }
    }
    return true;
  }
  bool isFusable(Node * node) {
    if (node->kind() == kFusionGroup) return true;
    return isSimpleMap(node) && allFloatIO(node) && isCuda(node);
  }

  // Can this node produce an _output_ of a fusion group?
  // all Fusable nodes can do this, but additionally Concat, which normally cannot be fused
  // because it is not a simple map, can be put in a fusion group
  // as long as no items in the group read the output of concat
  bool isFusableAsExitNode(Node * node) {
    if(isFusable(node))
      return true;
    if(node->kind() != kcat || !isCuda(node))
      return false;

    // this concat fusion only works when all the inputs are the same size
    // otherwise they cannot partipate in the same map
    auto sizes = node->input(0)->type()->expect<TensorType>()->sizes();
    for(auto i : node->inputs()) {
      if(sizes != i->type()->expect<TensorType>()->sizes()){
        return false;
      }
    }
    return true;
  }

  // necessary condition for fusion. If all of the uses of producer are consumer
  // then it is safe to merge producer into consumer, because it doesn't have any other uses
  // If there are other uses, but they occur _after_ consumer, then we can still merge in producer
  // with consumer, by rewriting those later uses to use the version of producer generated by the fused blob
  // In this case, producer becomes an output of the fusion group.
  bool allUsersAreThisConsumerOrOccurAfterIt(Node * consumer, Value * producer) {
    for(auto u : producer->uses()) {
      if(u.user != consumer && topological_index[consumer] > topological_index[u.user])
        return false;
    }
    return true;
  }
  bool allUsersAreThisConsumer(Node * consumer, Value * producer) {
    for(auto u : producer->uses()) {
      if(u.user != consumer)
        return false;
    }
    return true;
  }

  bool shouldFuse(Node * consumer, Value * producer) {
    // this handles cases where producer can be moved _into_ the fusion group of consumer.
    // TODO: extend to fusion of consumer into _producer's_ fusion blob
    // if the consumer allInputsAreThisProducer(consumer,producer)
    // we can move the consumer up into the producer.
    // but this requires better handling of merging fusion groups so it is not done now
    return isFusable(producer->node()) && allUsersAreThisConsumerOrOccurAfterIt(consumer, producer);
  }

  // insert a producer node into a consuming fusion group.
  // DOES NOT WORK if n is a consumer of an output of the fusion group
  // returns the node _inside_ the group that represents the node
  Graph & getSubgraph(Node * n) {
    JIT_ASSERT(n->kind() == kFusionGroup);
    return *n->g(kSubgraph);
  }

  void mergeFusionGroups(Node *consumer_group, Node *producer_group) {
    // Now we have two fusion groups!
    // Revert the fusion - place all inner nodes of producer back in the outer graph.
    std::vector<Node*> temporary_nodes;
    auto producer_subgraph = &getSubgraph(producer_group);

    // Initialize a map of inner graph values to outer graph values
    std::unordered_map<Value*, Value*> inner_to_outer;
    auto inner_inputs = producer_subgraph->inputs();
    auto outer_inputs = producer_group->inputs();
    for (std::size_t i = 0; i < inner_inputs.size(); ++i) {
      inner_to_outer[inner_inputs[i]] = outer_inputs[i];
    }

    // Clone all nodes
    for (auto inner : *producer_subgraph) {
      Node * outer = graph->createClone(inner, [&](Value * k) -> Value* {
        return inner_to_outer.at(k);
      });
      outer->insertBefore(producer_group);
      temporary_nodes.emplace_back(outer);
      auto inner_outputs = inner->outputs();
      auto outer_outputs = outer->outputs();
      for (std::size_t i = 0; i < inner_outputs.size(); ++i)
        inner_to_outer[inner_outputs[i]] = outer_outputs[i];
    }

    // Replace uses of producer_group outputs and destroy the producer
    auto subgraph_outputs = producer_subgraph->outputs();
    for (std::size_t i = 0; i < subgraph_outputs.size(); ++i) {
      auto outer_output = inner_to_outer.at(subgraph_outputs[i]);
      producer_group->outputs()[i]->replaceAllUsesWith(outer_output);
    }
    producer_group->destroy();
    producer_group = nullptr; // Just to get a clear error in case someone uses it

    // Inline the temporary nodes into the first group
    auto consumer_subgraph = &getSubgraph(consumer_group);
    for (auto it = temporary_nodes.rbegin(); it != temporary_nodes.rend(); ++it) {
      Node *node = *it;
      Node *merged = mergeNodeIntoGroup(consumer_group, node);
      // If any of the outputs are still used then we need to add them
      auto outputs = node->outputs();
      for (std::size_t i = 0; i < outputs.size(); ++i) {
        auto output = outputs[i];
        if (output->uses().size() == 0) continue;
        consumer_subgraph->registerOutput(merged->outputs()[i]);
        auto new_output = consumer_group->addOutput();
        output->replaceAllUsesWith(new_output);
        new_output->setType(output->typeOption());
      }
      node->destroy();
    }
  }

  Node * mergeNodeIntoGroup(Node* group, Node * n) {
    JIT_ASSERT(n->kind() != kFusionGroup);
    auto & subgraph = getSubgraph(group);
    // map from nodes in the surrounding graph to parameters in the fusion
    // group's subgraph that correspond to them
    std::unordered_map<Value*,Value*> inputs_map;
    size_t i = 0;
    JIT_ASSERT(group->inputs().size() == subgraph.inputs().size());
    for(auto input : group->inputs()) {
      inputs_map[input] = subgraph.inputs()[i++];
    }
    // add n's inputs to the fusion group's input list if we don't already have them
    for (auto input : n->inputs()) {
      if (inputs_map.count(input) == 0) {
        auto in_group = subgraph.addInput();
        in_group->setType(input->typeOption());
        inputs_map[input] = in_group;
        group->addInput(input);
      }
    }
    // copy n into the graph, remapping its inputs to internal nodes
    Node * in_graph = subgraph.createClone(n,[&](Value * k)-> Value* {
      return inputs_map[k];
    });
    // if n is already an input to the fusion group,
    // we need to remove it because n is now inside the fusion group
    // remapping nodes that used the input to the newly-merged node
    // n is not an input when the fusion group is empty
    auto inputs = group->inputs();
    auto it = std::find(inputs.begin(), inputs.end(), n->output());
    if(it != inputs.end()) {
      size_t p = it - inputs.begin();
      group->removeInput(p);
      subgraph.inputs()[p]->replaceAllUsesWith(in_graph->output());
      subgraph.eraseInput(p);
    }
    return subgraph.prependNode(in_graph);
  }

  // turn consumer node n into a fusion group with just n inside
  // to prepare for fusion and replace uses of n with the new group
  Node * createSingletonFusionGroup(Node * n) {
    auto group = graph->createFusionGroup();
    // propogate position information for the new node so we can always
    // have a valid mapping
    topological_index[group] = topological_index[n];
    group->insertBefore(n);
    Node * mergedNode = mergeNodeIntoGroup(group,n);
    getSubgraph(group).registerOutput(mergedNode->output());
    auto sel = group->addOutput();
    sel->copyMetadata(n->output());
    n->replaceAllUsesWith(group);
    n->destroy();
    return group;
  }
  void insertAfter(Node * n, Node * after) {
    n->insertAfter(after);
    topological_index[n] = topological_index[after];
  }

  void insertAt(Node ** insertion_point, Node * n) {
    insertAfter(n, *insertion_point);
    *insertion_point = n;
  }

  Node * fuse(Node * consumer, Value * producer) {
    auto group = consumer;
    if(group->kind() != kFusionGroup) {
      group = createSingletonFusionGroup(consumer);
    }
    if (producer->node()->kind() == kFusionGroup) {
      mergeFusionGroups(group, producer->node());
      return group;
    }
    Node * merged = mergeNodeIntoGroup(group, producer->node());
    // remaining uses of this producer can occur because we allow
    // fusion in cases where uses remain after the consumer
    // if these exist, re-route them to the version of producer
    // created in FusionGroup
    if(producer->uses().size() != 0) {
      getSubgraph(group).registerOutput(merged->output());
      Value * new_producer = group->addOutput();
      new_producer->copyMetadata(producer);
      producer->replaceAllUsesWith(new_producer);
    }
    producer->node()->destroy();
    return group;
  }

  bool isChunk(Node * node) {
    return node->kind() == ksplit;
  }

  // in places where op can be fused into a consumer but chunk is in the way
  // distribute chunk to op's operands:
  // replace a,b = chunk(op(x,y,z)) with:
  // x0,x1 = chunk(x) (x0 has a's type, x1 has b's type)
  // y0,y1 = chunk(y) (y0 has a's type, y1 has b's type)
  // z0,z1 = chunk(z) (z0 has a's type, z1 has b's type)
  // a = op(x0,y0,z0) (a,b have their same size but are now contiguous)
  // b = op(x1,y1,x1)
  //
  // NB: Chunk motion only occurs with fusable consumers, which implies
  // that there is always some other operation, e.g., a+b, that happens
  // after the chunk, and will be put into the fusion group. This is
  // important, because distributing the chunk changes the contiguity
  // of a and b, and so the results would be invalid, except that we know
  // that simple_mappable operations will restore contiguity before
  // we exit the fusion group.

  bool tryToMoveChunk(Node * consumer, Value * producer) {
    // is the output from a chunk node?
    auto * chunk = producer->node();
    if (!isChunk(chunk))
      return false;
    // and the thing being chunked is fusable into the consumer
    Value * producer_for_chunk = chunk->input();
    if (!isFusable(producer_for_chunk->node()) || !allUsersAreThisConsumer(chunk,producer_for_chunk))
      return false;
    // and all uses of the chunk are in this consumer
    for (auto s : chunk->outputs()) {
      for (auto u : s->uses()) {
        if (u.user != consumer)
          return false;
      }
    }

    // TODO: Remove this restriction if we ever need to distribute across
    // multiple return operators
    Node * producer_for_chunk_node = producer_for_chunk->node();
    JIT_ASSERT(producer_for_chunk_node->outputs().size() == 1);
    // Make sure we lay out the nodes in the correct topological order.
    // TODO: There should be some more enshrined way to do this
    Node * insertion_point = chunk;

    // apply chunk to each of op's operands
    // chunked_inputs[input_nr][chunk_output_idx]
    //  = Node* for chunk_output_idx'th output of the chunk(inputs[input_nr])
    std::vector<std::vector<Value*>> chunked_inputs;
    for (auto input : producer_for_chunk_node->inputs()) {
      auto input_type = input->type()->cast<TensorType>();
      // NB: I decided not to use cloneFrom here, because if we make cloneFrom
      // copy selects one day, it is definitely not what you want here (selects
      // have different types).
      Node * input_chunk = graph->create(ksplit, 0);
      input_chunk->copyAttributes(*chunk);
      input_chunk->addInput(input);
      insertAt(&insertion_point, input_chunk);

      chunked_inputs.emplace_back(); // alas, to not be C++17
      for (auto chunk_sel : chunk->outputs()) {
          auto chunk_sel_type = chunk_sel->type()->cast<TensorType>();
          Value * input_chunk_sel = input_chunk->addOutput();
          input_chunk_sel->setType(
            input_type->withSizesStrides(chunk_sel_type->sizes(),
                                         chunk_sel_type->strides()));
          chunked_inputs.back().push_back(input_chunk_sel);
      }
    }

    // apply the op to each chunk of the chunked operands,
    // and then rewrite the graph to use them!
    for (auto chunk_sel : chunk->outputs()) {
      Node * chunked_op = graph->create(producer_for_chunk_node->kind());
      chunked_op->copyAttributes(*producer_for_chunk_node);
      // Invariant: mappable operators always produce contiguous output
      chunked_op->output()->setType(chunk_sel->type()->cast<TensorType>()->contiguous());
      for (auto by_chunk_output_idx : chunked_inputs) {
        chunked_op->addInput(by_chunk_output_idx.at(chunk_sel->offset()));
      }
      insertAt(&insertion_point, chunked_op);
      chunk_sel->replaceAllUsesWith(chunked_op->output());
    }
    chunk->destroy();
    producer_for_chunk_node->destroy();
    return true;
  }

  // returns where to continue scanning, and whether any fusion was made
  std::pair<graph_node_list::iterator, bool> scanNode(Node * consumer) {
    auto stage_guard = graph->setStageTemporary(consumer->stage());
    if(isFusableAsExitNode(consumer)) {
      // handle inputs in reverse topological order as well...
      // otherwise in f(a,a+b) it will appear a is used twice if we consider
      // the f-a fusion before the f-(a+b) fusion first.
      value_list inputs = consumer->inputs();
      for(auto i : inputs) {
        JIT_ASSERT(topological_index.count(i->node()) > 0);
      }
      std::sort(inputs.begin(), inputs.end(), [&](Value * a, Value * b) {
        return topological_index[a->node()] > topological_index[b->node()];
      });
      for(auto producer : inputs) {
        // Don't fuse accross stage boundaries
        if (producer->stage() != consumer->stage()) continue;
        if(tryToMoveChunk(consumer,producer)) {
          // the chunk before this consumer was re-arranged to allow fusion,
          // we scan this consumer again to perform the fusion
          return std::make_pair(consumer->reverseIterator(), true);
        }
        if(shouldFuse(consumer, producer)) {
          auto fusion_group = fuse(consumer,producer);
          // after fusion, consumer moves into a FusionGroup, so inputs is no longer valid
          // so we rescan the new FusionGroup for more fusions...
          return std::make_pair(fusion_group->reverseIterator(), true);
        }
      }
    }
    return std::make_pair(++consumer->reverseIterator(), false);
  }

  void run() {
    for(auto p : graph->inputs()) {
      topological_index[p->node()] = 0;
    }
    size_t i = 1;
    auto nodes = graph->nodes();
    for(auto consumer : nodes) {
      topological_index[consumer] = i++;
    }
    topological_index[graph->return_node()] = i++;

    // Run the pass until no changes are made.
    // This is neccessary, because the algorithm can miss out on certain fusion
    // opportunities if ran only once. Consider this graph:
    //
    // %1 = f(...)
    // %2 = g(%1)
    // %3 = h(%1)
    // %4 = l(%3)
    // return (%4, %2)
    //
    // where f, g, h, l are simple map ops.
    // The first iteration will fuse %4 and %3, and see that %1 is an input, but
    // can't be fused, because it has a different use before the fusion group
    // in our topological ordering. Then, %2 will be considered, and fused with %1.
    // If we do another iteration, the algorithm will consider the fusion of these
    // two groups and fix the situation.
    bool any_changed = true;
    while (any_changed) {
      any_changed = false;
      for (auto it = nodes.rbegin(); it != nodes.rend();) {
        bool changed;
        std::tie(it, changed) = scanNode(*it);
        any_changed |= changed;
      }
    }
  }
};

} // anonymous namespace

void FuseGraph(std::shared_ptr<Graph>& graph) {
  GraphFuser(graph).run();
}

}}
