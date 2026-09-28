#ifndef DAG_HPP
#define DAG_HPP

#include <vector>
#include <queue>

/**
 * @brief Directed graph of pipeline subtasks and their dependency edges.
 *
 * Stores nodes (each wrapping a subtask id plus its predecessor/successor
 * ids) and offers graph queries used to validate and schedule a pipeline:
 * topological ordering, cycle detection, depth, and fan-in/fan-out counts.
 * Purely topology — DAG has no notion of the Component layer.
 */
class DAG {
public:
    /**
     * @brief A single pipeline subtask and its adjacency information.
     *
     * @var Node::id Unique identifier for this node.
     * @var Node::predecessors Ids of nodes with an edge into this node.
     * @var Node::successors Ids of nodes this node has an edge into.
     */
    struct Node {
        int id;
        std::vector<int> predecessors;
        std::vector<int> successors;
    };

    /** @brief Constructs an empty DAG with no nodes or edges. */
    DAG() {}

    /**
     * @brief Appends a new node to the graph.
     * @param id Unique identifier for the new node.
     * @return void
     */
    void add_node(int id);

    /**
     * @brief Adds a directed edge between two existing nodes.
     * @param from Id of the source node; @p to is recorded as its successor.
     * @param to Id of the destination node; @p from is recorded as its
     *        predecessor.
     * @return void
     */
    void add_edge(int from, int to);

    /**
     * @brief Computes a valid execution order via Kahn's algorithm.
     * @return Node ids in an order that respects all dependency edges.
     * @throws std::runtime_error if the graph contains a cycle.
     */
    std::vector<int> topological_sort() const;

    /**
     * @brief Checks whether the graph contains a cycle.
     * @return true if a cycle exists, false if the graph is a valid DAG.
     */
    bool has_cycle() const;

    /**
     * @brief Computes the length of the longest path in the DAG.
     * @return Number of nodes on the longest dependency chain.
     * @throws std::runtime_error if the graph contains a cycle.
     */
    int pipeline_depth() const;

    /**
     * @brief Counts the direct predecessors of a node.
     * @param id Id of the node to query.
     * @return Number of direct predecessors, or -1 if @p id is not found.
     */
    int fan_in_count(int id) const;

    /**
     * @brief Counts the direct successors of a node.
     * @param id Id of the node to query.
     * @return Number of direct successors, or -1 if @p id is not found.
     */
    int fan_out_count(int id) const;

    /**
     * @brief Read-only access to all nodes, used by TeamManager for wiring.
     * @return Const reference to the internal node list.
     */
    const std::vector<Node>& nodes() const { return nodes_; }

private:
    std::vector<Node> nodes_; // List of subtasks (nodes) in the DAG

    /**
     * @brief Looks up a node by id.
     * @param id Id of the node to find.
     * @return Pointer to the matching node, or nullptr if not found.
     */
    const Node* find_node(int id) const;
};

#endif // DAG_HPP
