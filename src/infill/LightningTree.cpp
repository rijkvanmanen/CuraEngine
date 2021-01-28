//Copyright (c) 2021 Ultimaker B.V.
//CuraEngine is released under the terms of the AGPLv3 or higher.

#include "LightningTree.h"

#include "../utils/linearAlg2D.h"

using namespace cura;

coord_t LightningTreeNode::getWeightedDistance(const Point& unsupported_loc, const coord_t& supporting_radius) const
{
    size_t valence = (!is_root) + children.size();
    coord_t boost = (0 < valence && valence < 4) ? 4 * supporting_radius : 0;
    return vSize(getLocation() - unsupported_loc) - boost;
}

bool LightningTreeNode::hasOffspring(const std::shared_ptr<LightningTreeNode>& to_be_checked) const
{
    if (to_be_checked == shared_from_this()) return true;
    for (auto child_ptr : children)
    {
        if (child_ptr->hasOffspring(to_be_checked)) return true;
    }
    return false;
}

const Point& LightningTreeNode::getLocation() const
{
    return p;
}

void LightningTreeNode::setLocation(const Point& loc)
{
    p = loc;
}

std::shared_ptr<LightningTreeNode> LightningTreeNode::addChild(const Point& child_loc)
{
    assert(p != child_loc);
    children.push_back(LightningTreeNode::create(child_loc));
    return children.back();
}

std::shared_ptr<LightningTreeNode> LightningTreeNode::addChild(std::shared_ptr<LightningTreeNode>& new_child)
{
    assert(new_child != shared_from_this());
    //     assert(p != new_child->p);
    if (p == new_child->p)
        std::cerr << "wtf\n";
    children.push_back(new_child);
    new_child->is_root = false;
    return new_child;
}

std::shared_ptr<LightningTreeNode> LightningTreeNode::findClosestNode(const Point& x, const coord_t& supporting_radius)
{
    coord_t closest_distance = getWeightedDistance(x, supporting_radius);
    std::shared_ptr<LightningTreeNode> closest_node = shared_from_this();
    findClosestNodeHelper(x, supporting_radius, closest_distance, closest_node);
    return closest_node;
}

void LightningTreeNode::propagateToNextLayer
(
    std::vector<std::shared_ptr<LightningTreeNode>>& next_trees,
    const Polygons& next_outlines,
    const coord_t& prune_distance,
    const coord_t& smooth_magnitude
) const
{
    auto tree_below = deepCopy();

    // TODO: What is the correct order of the following operations?
    //       (NOTE: in case realign turns out _not_ to be last, would need to rewrite a few things, see the 'rerooted_parts' parameter of that function).
    tree_below->prune(prune_distance);
    tree_below->straighten(smooth_magnitude);
    if (tree_below->realign(next_outlines, next_trees))
    {
        next_trees.push_back(tree_below);
    }
}

// NOTE: Depth-first, as currently implemented.
//       Skips the root (because that has no root itself), but all initial nodes will have the root point anyway.
void LightningTreeNode::visitBranches(const branch_visitor_func_t& visitor) const
{
    for (const auto& node : children)
    {
        visitor(p, node->p);
        node->visitBranches(visitor);
    }
}

// NOTE: Depth-first, as currently implemented.
void LightningTreeNode::visitNodes(const node_visitor_func_t& visitor)
{
    visitor(shared_from_this());
    for (const auto& node : children)
    {
        node->visitNodes(visitor);
    }
}

// Node:
LightningTreeNode::LightningTreeNode(const Point& p) : is_root(false), p(p) {}

// Root (and Trunk):
LightningTreeNode::LightningTreeNode(const Point& a, const Point& b) : LightningTreeNode(a)
{
    children.push_back(LightningTreeNode::create(b));
    is_root = true;
}

void LightningTreeNode::findClosestNodeHelper(const Point& x, const coord_t supporting_radius, coord_t& closest_distance, std::shared_ptr<LightningTreeNode>& closest_node)
{
    for (const auto& node : children)
    {
        node->findClosestNodeHelper(x, supporting_radius, closest_distance, closest_node);
        const coord_t distance = node->getWeightedDistance(x, supporting_radius);
        if (distance < closest_distance)
        {
            closest_node = node;
            closest_distance = distance;
        }
    }
}

std::shared_ptr<LightningTreeNode> LightningTreeNode::deepCopy() const
{
    std::shared_ptr<LightningTreeNode> local_root = LightningTreeNode::create(p);
    local_root->is_root = is_root;
    local_root->children.reserve(children.size());
    for (const auto& node : children)
    {
        local_root->children.push_back(node->deepCopy());
    }
    return local_root;
}

bool LightningTreeNode::realign(const Polygons& outlines, std::vector<std::shared_ptr<LightningTreeNode>>& rerooted_parts, const bool& connected_to_parent)
{
    // TODO: Hole(s) in the _middle_ of a line-segement, not unlikely since reconnect.
    // TODO: Reconnect if not on outline -> plan is: yes, but not here anymore!

    if (outlines.empty())
    {
        return false;
    }

    if (outlines.inside(p, true))
    {
        // Only keep children that have an unbroken connection to here, realign will put the rest in rerooted parts due to recursion:
        const std::function<bool(const std::shared_ptr<LightningTreeNode>& child)> remove_unconnected_func
        (
            [&outlines, &rerooted_parts](const std::shared_ptr<LightningTreeNode>& child)
            {
                constexpr bool argument_with_connected = true;
                return !child->realign(outlines, rerooted_parts, argument_with_connected);
            }
        );
        children.erase(std::remove_if(children.begin(), children.end(), remove_unconnected_func), children.end());
        return true;
    }

    // 'Lift' any decendants out of this tree:
    for (auto& child : children)
    {
        constexpr bool argument_with_disconnect = false;
        if (child->realign(outlines, rerooted_parts, argument_with_disconnect))
        {
            rerooted_parts.push_back(child);
        }
    }
    children.clear();

    if (connected_to_parent)
    {
        // This will now be a (new_ leaf:
        p = PolygonUtils::findClosest(p, outlines).p();
        return true;
    }

    return false;
}

void LightningTreeNode::straighten(const coord_t& magnitude)
{
    straighten(magnitude, p, 0);
}

LightningTreeNode::RectilinearJunction LightningTreeNode::straighten(const coord_t& magnitude, Point junction_above, coord_t accumulated_dist)
{
    const coord_t junction_magnitude = magnitude * 3 / 4;
    if (children.size() == 1)
    {
        auto child_p = children.front();
        coord_t child_dist = vSize(p - child_p->p);
        RectilinearJunction junction_below = child_p->straighten(magnitude, junction_above, accumulated_dist + child_dist);
        coord_t total_dist_to_junction_below = junction_below.total_recti_dist;
        Point a = junction_above;
        Point b = junction_below.junction_loc;
        if (a != b) // should always be true!
        {
            Point ab = b - a;
            Point destination = a + ab * accumulated_dist / total_dist_to_junction_below;
            if (shorterThen(destination - p, magnitude))
            {
                p = destination;
            }
            else
            {
                p = p + normal(destination - p, magnitude);
            }
        }
        return junction_below;
    }
    else
    {
        coord_t small_branch = 800;
        auto weight = [magnitude, small_branch](coord_t d) { return std::max(10 * (small_branch - d), coord_t(std::sqrt(small_branch * d))); };
        Point junction_moving_dir = normal(junction_above - p, weight(accumulated_dist));
        for (auto child_p : children)
        {
            coord_t child_dist = vSize(p - child_p->p);
            RectilinearJunction below = child_p->straighten(magnitude, p, child_dist);

            junction_moving_dir += normal(below.junction_loc - p, weight(below.total_recti_dist));
        }
        if (junction_moving_dir != Point(0, 0) && ! children.empty())
        {
            coord_t junction_moving_dir_len = vSize(junction_moving_dir);
            if (junction_moving_dir_len > junction_magnitude)
            {
                junction_moving_dir = junction_moving_dir * junction_magnitude / junction_moving_dir_len;
            }
            p += junction_moving_dir;
        }
        return RectilinearJunction{ accumulated_dist, p };
    }
}

// Prune the tree from the extremeties (leaf-nodes) until the pruning distance is reached.
coord_t LightningTreeNode::prune(const coord_t& pruning_distance)
{
    if (pruning_distance <= 0)
    {
        return 0;
    }

    coord_t max_distance_pruned = 0;
    for (auto child_it = children.begin(); child_it != children.end(); )
    {
        auto& child = *child_it;
        coord_t dist_pruned_child = child->prune(pruning_distance);
        if (dist_pruned_child >= pruning_distance)
        { // pruning is finished for child; dont modify further
            max_distance_pruned = std::max(max_distance_pruned, dist_pruned_child);
            ++child_it;
        }
        else
        {
            Point a = getLocation();
            Point b = child->getLocation();
            Point ba = a - b;
            coord_t ab_len = vSize(ba);
            if (dist_pruned_child + ab_len <= pruning_distance)
            { // we're still in the process of pruning
                assert(child->children.empty() && "when pruning away a node all it's children must already have been pruned away");
                max_distance_pruned = std::max(max_distance_pruned, dist_pruned_child + ab_len);
                child_it = children.erase(child_it);
            }
            else
            { // pruning stops in between this node and the child
                Point n = b + normal(ba, pruning_distance - dist_pruned_child);
                assert(std::abs(vSize(n - b) + dist_pruned_child - pruning_distance) < 10 && "total pruned distance must be equal to the pruning_distance");
                max_distance_pruned = std::max(max_distance_pruned, pruning_distance);
                child->setLocation(n);
                ++child_it;
            }
        }
    }

    return max_distance_pruned;
}