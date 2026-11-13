#pragma once

#include <vector>

/* A tile's rectangle on the song grid, in cells */
struct Tile {
    int x = 0, y = 0, w = 1, h = 1;

    bool operator==(const Tile& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool operator!=(const Tile& o) const { return !(*this == o); }
};

/* Song grid layout: `count` tiles of 1×1, 2×1, 1×2 and 2×2 cells fill a fixed number of columns row by row,
   and empty cells remain only at the end of the last row. A tile's number is its index in Rest() */
class TileLayout {
public:
    TileLayout() = default;
    TileLayout(int columns, int count);

    int Columns() const { return columns_; }
    int Rows() const { return rows_; }
    const std::vector<Tile>& Rest() const { return rest_; }

    /* Tile covering the cell in the rest layout; -1 for an empty cell or one outside the grid */
    int TileAt(int x, int y) const;

    /* Positions of all tiles once `tile` grows into a 2×2 block over the cursor's cell, or the rest layout when the
       tiles around it cannot make room. `current` is where the tile is headed now; the cursor is in cells. Blocks
       within rows [viewTop, viewBottom), the ones fully on screen, are preferred. On a one-row grid the block
       reaches into a second row */
    std::vector<Tile> Expand(int tile, Tile current, float cursorX, float cursorY, int viewTop, int viewBottom) const;

private:
    bool Repack(int tile, Tile block, int margin, std::vector<Tile>& result) const;

    int columns_ = 1;
    int rows_ = 0;
    std::vector<Tile> rest_;
    std::vector<int> owner_;   // tile of each cell in reading order, -1 when empty
};
