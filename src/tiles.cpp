#include "tiles.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace {

/* Relative chance of each shape wherever it fits: mostly 1×1, some wide or tall tiles, a few 2×2 */
struct Shape {
    int w, h, weight;
};
const Shape kShapes[] = { { 2, 2, 3 }, { 2, 1, 12 }, { 1, 2, 12 }, { 1, 1, 73 } };

const int kMaxMargin = 5;   // how many cells the repack window reaches beyond the block

uint32_t Hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

bool Contains(Tile outer, Tile inner) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

Tile Intersect(Tile a, Tile b) {
    const int x0 = (std::max)(a.x, b.x), y0 = (std::max)(a.y, b.y);
    const int x1 = (std::min)(a.x + a.w, b.x + b.w), y1 = (std::min)(a.y + a.h, b.y + b.h);
    return { x0, y0, (std::max)(0, x1 - x0), (std::max)(0, y1 - y0) };
}

int OverlapArea(Tile a, Tile b) {
    const Tile common = Intersect(a, b);
    return common.w * common.h;
}

/* Fills the free cells of a window exactly with movable tiles, changing them as little as possible. Each tile keeps
   its size or shrinks to one cell wide or tall; a branch-and-bound search places them cell by cell in reading order */
class Repacker {
public:
    explicit Repacker(Tile window)
        : window_(window), occupied_((size_t)window.h, 0u), freeCells_(window.w * window.h) {}

    void Fill(Tile area) { SetOccupied(area, true); }

    void AddPiece(int tile, Tile home) {
        pieces_.push_back({ tile, home });
        unplacedArea_ += home.w * home.h;
    }

    /* Writes each tile's new place into `layout` on success. When the node budget runs out, the best
       arrangement found so far is used */
    bool Solve(std::vector<Tile>& layout) {
        unplaced_ = (int)pieces_.size();
        if (freeCells_ < unplaced_ || freeCells_ > unplacedArea_) return false;
        BuildCandidateOrder();
        current_.resize(pieces_.size());
        Search(0, 0);
        if (bestCost_ == INT_MAX) return false;
        for (size_t i = 0; i < pieces_.size(); ++i) layout[(size_t)pieces_[i].tile] = best_[i];
        return true;
    }

private:
    struct Piece {
        int tile;
        Tile home;
        bool placed = false;
    };

    static const int kChangeCost = 4;      // any change costs as much as moving two cells
    static const long kNodeLimit = 3000;   // bounds the time one expansion can take

    /* A changed tile costs kChangeCost plus how far its center moved, in half cells */
    static int PlacementCost(Tile home, Tile placed) {
        if (placed == home) return 0;
        const int dx = std::abs(2 * placed.x + placed.w - 2 * home.x - home.w);
        const int dy = std::abs(2 * placed.y + placed.h - 2 * home.y - home.h);
        return kChangeCost + dx + dy;
    }

    uint32_t RowMask(Tile t) const { return ((1u << t.w) - 1u) << (t.x - window_.x); }

    bool IsFree(Tile t) const {
        if (t.x + t.w > window_.x + window_.w || t.y + t.h > window_.y + window_.h) return false;
        const uint32_t mask = RowMask(t);
        for (int y = t.y; y < t.y + t.h; ++y)
            if (occupied_[(size_t)(y - window_.y)] & mask) return false;
        return true;
    }

    void SetOccupied(Tile t, bool occupied) {
        const uint32_t mask = RowMask(t);
        for (int y = t.y; y < t.y + t.h; ++y) {
            uint32_t& row = occupied_[(size_t)(y - window_.y)];
            row = occupied ? (row | mask) : (row & ~mask);
        }
        freeCells_ += (occupied ? -1 : 1) * t.w * t.h;
    }

    void Place(int piece, Tile t) {
        SetOccupied(t, true);
        pieces_[(size_t)piece].placed = true;
        current_[(size_t)piece] = t;
        --unplaced_;
        unplacedArea_ -= pieces_[(size_t)piece].home.w * pieces_[(size_t)piece].home.h;
    }

    void Unplace(int piece, Tile t) {
        SetOccupied(t, false);
        pieces_[(size_t)piece].placed = false;
        ++unplaced_;
        unplacedArea_ += pieces_[(size_t)piece].home.w * pieces_[(size_t)piece].home.h;
    }

    /* First free cell at or after `cell` in reading order, as an index within the window; -1 when all are full */
    int FirstFree(int cell) const {
        const uint32_t full = (1u << window_.w) - 1u;
        for (int y = cell / window_.w, x = cell % window_.w; y < window_.h; ++y, x = 0) {
            uint32_t freeBits = ~occupied_[(size_t)y] & full & ~((1u << x) - 1u);
            if (!freeBits) continue;
            int bit = 0;
            while (!(freeBits & 1u)) { freeBits >>= 1; ++bit; }
            return y * window_.w + bit;
        }
        return -1;
    }

    /* Each cell tries the tiles whose home is nearest first, so the first complete arrangement is already close
       to the original and prunes most of the search */
    void BuildCandidateOrder() {
        const int cells = window_.w * window_.h;
        const int count = (int)pieces_.size();
        order_.resize((size_t)cells * (size_t)count);
        for (int cell = 0; cell < cells; ++cell) {
            const int x = window_.x + cell % window_.w;
            const int y = window_.y + cell / window_.w;
            int* order = &order_[(size_t)cell * (size_t)count];
            for (int i = 0; i < count; ++i) order[i] = i;
            auto distance = [&](int piece) {
                const Tile& home = pieces_[(size_t)piece].home;
                return std::abs(home.x - x) + std::abs(home.y - y);
            };
            std::stable_sort(order, order + count, [&](int a, int b) { return distance(a) < distance(b); });
        }
    }

    /* A lower bound on the cost still to come: a tile whose home is taken must change, and when the free cells
       are fewer than the tiles' area, at least one tile has to shrink even if every home is free */
    int LowerBound() const {
        int changed = 0;
        for (const Piece& piece : pieces_)
            if (!piece.placed && !IsFree(piece.home)) ++changed;
        if (changed == 0 && freeCells_ < unplacedArea_) changed = 1;
        return changed * kChangeCost;
    }

    void Search(int cell, int cost) {
        if (++nodes_ > kNodeLimit) return;

        cell = FirstFree(cell);
        if (cell < 0) {
            if (unplaced_ == 0 && cost < bestCost_) {
                bestCost_ = cost;
                best_ = current_;
            }
            return;
        }
        // Every tile covers at least one cell and at most its original area
        if (freeCells_ < unplaced_ || freeCells_ > unplacedArea_) return;
        const int bound = LowerBound();
        if (cost + bound >= bestCost_) return;

        // All cells before this one are full, so the tile covering it has its top-left corner here
        const int x = window_.x + cell % window_.w;
        const int y = window_.y + cell / window_.w;
        const int* order = &order_[(size_t)cell * pieces_.size()];
        for (size_t i = 0; i < pieces_.size(); ++i) {
            const int piece = order[i];
            if (pieces_[(size_t)piece].placed) continue;
            const Tile home = pieces_[(size_t)piece].home;
            // A tile whose corner moves d cells costs at least 2 + 2d, and later tiles in the order are no closer
            const int d = std::abs(home.x - x) + std::abs(home.y - y);
            if (d > 0 && cost + (std::max)(bound, 2 + 2 * d) >= bestCost_) break;
            for (int h = home.h; h >= 1; --h) {
                for (int w = home.w; w >= 1; --w) {
                    const Tile t{ x, y, w, h };
                    if (!IsFree(t)) continue;
                    Place(piece, t);
                    Search(cell + 1, cost + PlacementCost(home, t));
                    Unplace(piece, t);
                }
            }
        }
    }

    Tile window_;
    std::vector<uint32_t> occupied_;   // one bit per cell, one mask per window row
    int freeCells_;
    std::vector<Piece> pieces_;
    int unplaced_ = 0;
    int unplacedArea_ = 0;
    std::vector<int> order_;
    std::vector<Tile> current_;
    std::vector<Tile> best_;
    int bestCost_ = INT_MAX;
    long nodes_ = 0;
};

}  // namespace

TileLayout::TileLayout(int columns, int count) : columns_(columns) {
    rest_.reserve((size_t)count);
    auto ownerAt = [&](int cell) { return cell < (int)owner_.size() ? owner_[(size_t)cell] : -1; };

    // Scan the cells in reading order and give each empty one a shape that fits, picked by a hash of the cell so the
    // layout is the same every time. A shape is ruled out when the tiles left could not fill the holes it leaves behind
    int frontier = 0;   // cells from here on are all empty
    int cell = 0;
    for (int tile = 0; tile < count; ++tile) {
        while (ownerAt(cell) >= 0) ++cell;
        const int x = cell % columns, y = cell / columns;
        auto allowed = [&](const Shape& s) {
            if (x + s.w > columns) return false;
            for (int dy = 0; dy < s.h; ++dy)
                for (int dx = 0; dx < s.w; ++dx)
                    if (ownerAt((y + dy) * columns + x + dx) >= 0) return false;
            const int end = (std::max)(frontier, (y + s.h - 1) * columns + x + s.w);
            int holes = 0;
            for (int i = cell + 1; i < end; ++i) {
                const int cx = i % columns, cy = i / columns;
                const bool covered = cx >= x && cx < x + s.w && cy >= y && cy < y + s.h;
                if (!covered && ownerAt(i) < 0) ++holes;
            }
            return holes <= count - tile - 1;
        };

        int total = 0;
        for (const Shape& s : kShapes)
            if (allowed(s)) total += s.weight;
        int pick = (int)(Hash32((uint32_t)cell) % (uint32_t)total);
        for (const Shape& s : kShapes) {
            if (!allowed(s)) continue;
            if (pick >= s.weight) { pick -= s.weight; continue; }
            rest_.push_back({ x, y, s.w, s.h });
            if ((int)owner_.size() < (y + s.h) * columns) owner_.resize((size_t)((y + s.h) * columns), -1);
            for (int dy = 0; dy < s.h; ++dy)
                for (int dx = 0; dx < s.w; ++dx)
                    owner_[(size_t)((y + dy) * columns + x + dx)] = tile;
            frontier = (std::max)(frontier, (y + s.h - 1) * columns + x + s.w);
            break;
        }
    }
    rows_ = (frontier + columns - 1) / columns;
    owner_.resize((size_t)(rows_ * columns), -1);
}

int TileLayout::TileAt(int x, int y) const {
    if (x < 0 || y < 0 || x >= columns_ || y >= rows_) return -1;
    return owner_[(size_t)(y * columns_ + x)];
}

std::vector<Tile> TileLayout::Expand(int tile, Tile current, float cursorX, float cursorY,
                                     int viewTop, int viewBottom) const {
    const int cellX = std::clamp((int)std::floor(cursorX), 0, columns_ - 1);
    const int cellY = std::clamp((int)std::floor(cursorY), 0, rows_ - 1);

    // Up to four 2×2 blocks cover the cursor's cell. Prefer those over fewer empty cells, then fewer rows off
    // screen, then the most overlap with where the tile is headed, then the center nearest the cursor
    struct Candidate {
        Tile block;
        int empty, hidden, overlap;
        float distance;
    };
    std::vector<Candidate> candidates;
    const int rowLimit = (std::max)(rows_, 2);
    for (int y = cellY - 1; y <= cellY; ++y) {
        for (int x = cellX - 1; x <= cellX; ++x) {
            if (x < 0 || y < 0 || x + 2 > columns_ || y + 2 > rowLimit) continue;
            candidates.emplace_back();
            Candidate& c = candidates.back();
            c.block = { x, y, 2, 2 };
            c.empty = (TileAt(x, y) < 0) + (TileAt(x + 1, y) < 0) + (TileAt(x, y + 1) < 0) + (TileAt(x + 1, y + 1) < 0);
            c.hidden = (y < viewTop) + (y + 1 < viewTop) + (y >= viewBottom) + (y + 1 >= viewBottom);
            c.overlap = OverlapArea(c.block, current);
            const float dx = x + 1 - cursorX, dy = y + 1 - cursorY;
            c.distance = dx * dx + dy * dy;
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.empty != b.empty) return a.empty < b.empty;
        if (a.hidden != b.hidden) return a.hidden < b.hidden;
        if (a.overlap != b.overlap) return a.overlap > b.overlap;
        return a.distance < b.distance;
    });

    std::vector<Tile> result = rest_;
    for (const Candidate& c : candidates)
        for (int margin = 1; margin <= kMaxMargin; ++margin)
            if (Repack(tile, c.block, margin, result)) {
                result[(size_t)tile] = c.block;
                return result;
            }
    return result;
}

/* Opens a window around the block and the tile's home. Tiles entirely inside it may move; tiles across its edge
   and empty cells stay. A margin of at least one keeps every tile that overlaps the block inside the window */
bool TileLayout::Repack(int tile, Tile block, int margin, std::vector<Tile>& result) const {
    const Tile& home = rest_[(size_t)tile];
    const int x0 = (std::max)(0, (std::min)(block.x, home.x) - margin);
    const int y0 = (std::max)(0, (std::min)(block.y, home.y) - margin);
    const int x1 = (std::min)(columns_, (std::max)(block.x + block.w, home.x + home.w) + margin);
    const int y1 = (std::min)((std::max)(rows_, block.y + block.h), (std::max)(block.y + block.h, home.y + home.h) + margin);
    const Tile window{ x0, y0, x1 - x0, y1 - y0 };

    Repacker repacker(window);
    repacker.Fill(block);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const int owner = TileAt(x, y);
            if (owner < 0) {
                if (!Contains(block, { x, y, 1, 1 })) repacker.Fill({ x, y, 1, 1 });
                continue;
            }
            // Each tile is handled once, at its first cell inside the window
            const Tile& other = rest_[(size_t)owner];
            if (owner == tile || x != (std::max)(other.x, x0) || y != (std::max)(other.y, y0)) continue;
            if (Contains(window, other)) repacker.AddPiece(owner, other);
            else repacker.Fill(Intersect(other, window));
        }
    }
    return repacker.Solve(result);
}
