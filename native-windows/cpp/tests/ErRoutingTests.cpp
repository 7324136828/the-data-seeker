#include "ErRouting.h"
#include "ErLayout.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <stdexcept>
#include <tuple>

using namespace native_app;
namespace {
void RequireRoute(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
bool Interior(ErRoutePoint a, ErRoutePoint b, const ErRouteRect& box) {
    if (a.y == b.y) return a.y > box.top && a.y < box.bottom && std::max(a.x, b.x) > box.left && std::min(a.x, b.x) < box.right;
    return a.x > box.left && a.x < box.right && std::max(a.y, b.y) > box.top && std::min(a.y, b.y) < box.bottom;
}
void CheckRoutes(const std::vector<ErRouteCard>& cards, const std::vector<ErRouteConnection>& edges, const ErRoutingResult& batch, bool requireAll = true) {
    RequireRoute(batch.routes.size() == edges.size(), "Router changed the input relationship order/count.");
    std::map<std::string, const ErRouteCard*> index; for (const auto& card : cards) index[card.id] = &card;
    for (std::size_t n = 0; n < edges.size(); ++n) {
        const auto& edge = edges[n]; const auto& route = batch.routes[n];
        if (!route.routed) { RequireRoute(!requireAll && route.points.empty() && !route.failure.empty(), "Unresolved route drew a fallback line or omitted its failure."); continue; }
        const auto& source = *index.at(edge.sourceId); const auto& target = *index.at(edge.targetId);
        RequireRoute(route.points.size() >= 2 && route.points.front() == route.source.point && route.points.back() == route.target.point, "Route endpoints do not match its declared ports.");
        RequireRoute(route.source.point.y == source.bounds.top + edge.sourceRowOffset && route.target.point.y == target.bounds.top + edge.targetRowOffset, "Connector port did not align to the exact requested row.");
        RequireRoute(route.source.point.x == (route.source.side == ErRouteSide::Left ? source.bounds.left : source.bounds.right) && route.target.point.x == (route.target.side == ErRouteSide::Left ? target.bounds.left : target.bounds.right), "Connector port is detached from the card side.");
        const auto first = route.points[1], last = route.points[route.points.size() - 2];
        RequireRoute(first.y == route.source.point.y && (route.source.side == ErRouteSide::Left ? first.x < route.source.point.x : first.x > route.source.point.x), "Connector leaves a source card in the wrong direction.");
        RequireRoute(last.y == route.target.point.y && (route.target.side == ErRouteSide::Left ? last.x < route.target.point.x : last.x > route.target.point.x), "Connector enters a target card in the wrong direction.");
        for (std::size_t i = 1; i < route.points.size(); ++i) {
            const auto a = route.points[i - 1], b = route.points[i];
            RequireRoute(a.x >= 0 && a.y >= 0 && b.x >= 0 && b.y >= 0 && !(a == b) && (a.x == b.x || a.y == b.y), "Connector is negative, diagonal, or has an empty segment.");
            for (const auto& card : cards) RequireRoute(!Interior(a, b, card.bounds), "Connector crosses a card interior.");
            RequireRoute(batch.extentRight >= a.x && batch.extentRight >= b.x && batch.extentBottom >= a.y && batch.extentBottom >= b.y, "Router extents omit a connector segment.");
        }
    }
}
bool Same(const ErConnectorRoute& a, const ErConnectorRoute& b) { return a.routed == b.routed && a.source.point == b.source.point && a.target.point == b.target.point && a.source.side == b.source.side && a.target.side == b.target.side && a.points == b.points && a.failure == b.failure; }
}

void TestNativeErRoutingGeometry() {
    const std::vector<ErLayoutNode> ecommerceNodes{{"categories",235,38+24*5},{"customers",235,38+24*7},{"order_items",235,38+24*6},{"orders",235,38+24*7},{"products",235,38+24*8},{"reviews",235,38+24*6}};
    const std::vector<ErLayoutEdge> ecommerceLinks{{"products","categories"},{"orders","customers"},{"order_items","orders"},{"order_items","products"},{"reviews","products"},{"reviews","customers"}};
    const auto samplePositions=ComputeErLayout(ecommerceNodes,ecommerceLinks);
    std::vector<ErRouteCard> sampleCards;for(const auto& node:ecommerceNodes){const auto point=samplePositions.at(node.id);sampleCards.push_back({node.id,{point.x,point.y,point.x+node.width,point.y+node.height}});}
    const std::vector<ErRouteConnection> sampleConnections{{"products","categories",68,44},{"orders","customers",68,44},{"order_items","orders",68,44},{"order_items","products",92,44},{"reviews","products",68,44},{"reviews","customers",92,44}};
    const auto sampleRoutes=RouteErConnections(sampleCards,sampleConnections);CheckRoutes(sampleCards,sampleConnections,sampleRoutes);RequireRoute(!sampleRoutes.limited,"The real eCommerce sample exhausted the default routing budget.");
    std::vector<ErRouteCard> cards{{"a",{100,100,220,230}},{"b",{500,100,620,230}}};
    std::vector<ErRouteConnection> edges{{"a","b",44,68}};
    const auto forward = RouteErConnections(cards, edges); CheckRoutes(cards, edges, forward);
    RequireRoute(forward.routes[0].source.side == ErRouteSide::Right && forward.routes[0].target.side == ErRouteSide::Left, "Forward connector selected backward card ports.");
    cards[0].bounds = {700,100,820,230}; cards[1].bounds = {50,100,170,230};
    const auto reversed = RouteErConnections(cards, edges); CheckRoutes(cards, edges, reversed);
    RequireRoute(reversed.routes[0].source.side == ErRouteSide::Left && reversed.routes[0].target.side == ErRouteSide::Right, "Moving tables did not reverse connector side choice.");
    cards = {{"a",{100,100,220,230}},{"b",{500,100,620,230}},{"obstacle",{300,50,420,330}}};
    const auto obstacle = RouteErConnections(cards, edges); CheckRoutes(cards, edges, obstacle);
    RequireRoute(std::any_of(obstacle.routes[0].points.begin(), obstacle.routes[0].points.end(), [](auto point) { return point.y <= 38 || point.y >= 342; }), "Connector did not clear the tall intervening obstacle.");
    cards = {{"a",{0,0,120,130}},{"b",{0,300,120,430}}};
    const auto column = RouteErConnections(cards, edges); CheckRoutes(cards, edges, column);
    RequireRoute(column.routes[0].source.side == ErRouteSide::Right && column.routes[0].target.side == ErRouteSide::Right, "Same-column cards at the canvas boundary used negative left ports.");
    edges = {{"a","a",44,68},{"a","a",44,44},{"b","a",44,92}};
    const auto self = RouteErConnections(cards, edges); CheckRoutes(cards, edges, self);
    RequireRoute(self.routes[1].points.size() >= 5, "Same-row self relationship collapsed to an empty connector.");
    cards = {{"a",{100,100,220,230}},{"b",{500,100,620,230}}};
    edges.assign(12, {"a","b",44,68});
    const auto parallel = RouteErConnections(cards, edges); CheckRoutes(cards, edges, parallel);
    for (std::size_t i = 1; i < parallel.routes.size(); ++i) RequireRoute(parallel.routes[i].points != parallel.routes[i - 1].points, "Parallel relationships reused an identical path instead of another lane.");
    const auto again = RouteErConnections(cards, edges);
    for (std::size_t i = 0; i < edges.size(); ++i) RequireRoute(Same(parallel.routes[i], again.routes[i]), "Routing is nondeterministic for identical input.");
    cards.clear(); edges.clear();
    for (int row = 0; row < 3; ++row) for (int columnIndex = 0; columnIndex < 4; ++columnIndex) cards.push_back({"n" + std::to_string(row * 4 + columnIndex), {40 + columnIndex * 210,40 + row * 190,160 + columnIndex * 210,170 + row * 190}});
    for (int i = 0; i < 12; ++i) { edges.push_back({"n" + std::to_string(i),"n" + std::to_string((i + 5) % 12),44,68}); edges.push_back({"n" + std::to_string(i),"n" + std::to_string((i + 9) % 12),92,44}); }
    const auto start = std::chrono::steady_clock::now(); const auto dense = RouteErConnections(cards, edges); CheckRoutes(cards, edges, dense);
    RequireRoute(dense.expandedStates <= ErRoutingOptions{}.maximumExpandedStatesTotal && std::chrono::steady_clock::now() - start < std::chrono::seconds(5), "Dense routing exceeded its total search/performance bound.");
    auto permutedCards = cards; auto permutedEdges = edges; std::reverse(permutedCards.begin(), permutedCards.end()); std::reverse(permutedEdges.begin(), permutedEdges.end()); const auto permuted = RouteErConnections(permutedCards, permutedEdges);
    for (std::size_t i = 0; i < edges.size(); ++i) RequireRoute(Same(dense.routes[i], permuted.routes[edges.size() - 1 - i]), "Card/relationship enumeration order changed deterministic geometry.");
    ErRoutingOptions tiny; tiny.maximumExpandedStatesTotal = 1; const auto limited = RouteErConnections(cards, edges, tiny); CheckRoutes(cards, edges, limited, false); RequireRoute(limited.limited && limited.expandedStates <= 1, "Batch routing ignored its total expansion budget.");
    tiny = {}; tiny.maximumGridVertices = 2; const auto gridLimit = RouteErConnections(cards, edges, tiny); CheckRoutes(cards, edges, gridLimit, false); RequireRoute(gridLimit.limited, "Oversized routing grid did not report its limit.");
    cards = {{"a",{100,100,220,230}},{"cover",{80,80,240,250}},{"b",{500,100,620,230}}}; edges = {{"a","b",44,68}};
    const auto overlap = RouteErConnections(cards, edges); CheckRoutes(cards, edges, overlap, false); RequireRoute(!overlap.routes[0].routed, "Router drew through an overlapping endpoint card.");
    edges = {{"a","missing",44,68},{"a","b",999,68}}; const auto invalid = RouteErConnections(cards, edges); CheckRoutes(cards, edges, invalid, false);
    RequireRoute(!invalid.routes[0].routed && !invalid.routes[1].routed, "Missing or invalid row endpoints were routed.");
}
