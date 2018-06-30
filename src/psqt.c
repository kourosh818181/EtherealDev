/*
  Ethereal is a UCI chess playing engine authored by Andrew Grant.
  <https://github.com/AndyGrant/Ethereal>     <andrew@grantnet.us>

  Ethereal is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Ethereal is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "assert.h"
#include "bitboards.h"
#include "evaluate.h"
#include "psqt.h"
#include "types.h"

int PSQT[32][SQUARE_NB];

#define S(mg, eg) MakeScore((mg), (eg))

const int PawnPSQT32[32] = {
    S(   0,   0), S(   0,   0), S(   0,   0), S(   0,   0),
    S( -23,   2), S(  13,   3), S(  -6,   6), S(  -5,   0),
    S( -26,   0), S(  -4,  -1), S(  -6,  -6), S(  -2, -11),
    S( -21,   7), S(  -7,   6), S(   5, -10), S(   3, -23),
    S( -10,  15), S(   3,   9), S(   0,  -2), S(   4, -23),
    S(   1,  29), S(  13,  28), S(  18,   6), S(  24, -21),
    S( -46,   7), S( -34,  11), S(  -4, -16), S(   1, -33),
    S(   0,   0), S(   0,   0), S(   0,   0), S(   0,   0),
};

const int KnightPSQT32[32] = {
    S( -57, -63), S(   6, -42), S( -21, -26), S(   1, -17),
    S(   0, -51), S(   4, -15), S(   6, -30), S(  22,  -8),
    S(   0, -27), S(  23, -20), S(  13,   0), S(  30,  11),
    S(  10,   4), S(  23,   6), S(  26,  27), S(  32,  30),
    S(  27,   5), S(  33,  14), S(  38,  37), S(  41,  38),
    S( -18,  13), S(  34,   7), S(  42,  35), S(  55,  33),
    S( -12, -10), S( -19,  11), S(  66, -19), S(  35,   3),
    S(-151, -25), S( -71, -13), S(-138,   7), S( -12,  -7),
};

const int BishopPSQT32[32] = {
    S(  26, -28), S(  17, -30), S(  -4, -13), S(  11, -21),
    S(  36, -33), S(  35, -26), S(  27, -16), S(   7,  -5),
    S(  23, -16), S(  35, -13), S(  22,   0), S(  21,   5),
    S(  23,  -4), S(  14,   0), S(  12,  13), S(  29,  13),
    S( -18,   7), S(  23,   4), S(   5,  14), S(  30,  16),
    S(  -2,   3), S(   5,   7), S(  32,  11), S(  23,   8),
    S( -45,   6), S(   0,   0), S(   0,  -5), S( -24,   7),
    S( -50,  -2), S( -61,  -3), S(-120,   7), S(-110,  11),
};

const int RookPSQT32[32] = {
    S(  -4, -32), S(  -6, -18), S(   5, -14), S(  11, -20),
    S( -35, -25), S(  -6, -28), S(   2, -20), S(  10, -26),
    S( -20, -19), S(   4, -14), S(  -1, -18), S(   2, -20),
    S( -21,  -1), S( -12,   4), S(  -4,   2), S(  -2,   2),
    S( -14,  11), S( -13,   9), S(  16,   5), S(  19,   6),
    S( -18,  14), S(  15,   9), S(  11,  13), S(  18,  13),
    S(  -3,  16), S(  -9,  16), S(  36,   2), S(  20,   8),
    S(   0,  22), S(  11,  13), S( -24,  22), S(   3,  27),
};

const int QueenPSQT32[32] = {
    S(  -1, -47), S( -10, -30), S(  -3, -21), S(  17, -41),
    S(   7, -49), S(  15, -37), S(  21, -52), S(  16, -15),
    S(   7, -23), S(  23, -18), S(   7,   6), S(   4,   4),
    S(   6,  -6), S(   8,   4), S(  -6,  15), S(  -8,  46),
    S( -14,  10), S( -15,  33), S(  -9,  22), S( -25,  52),
    S( -15,   3), S(  -6,  19), S(  -1,  21), S( -11,  46),
    S(  -7,  12), S( -76,  55), S(  23,  11), S( -21,  67),
    S( -22, -24), S(   2, -14), S(   8,  -6), S( -20,   9),
};

const int KingPSQT32[32] = {
    S(  81,-106), S(  89, -80), S(  40, -35), S(  22, -39),
    S(  71, -54), S(  60, -45), S(  10,  -5), S( -21,   3),
    S(   0, -41), S(  44, -31), S(  16,  -1), S( -15,  16),
    S( -53, -33), S(  33, -19), S(   1,  15), S( -47,  37),
    S( -19, -18), S(  56,   2), S(   8,  31), S( -32,  38),
    S(  40, -17), S(  85,   0), S(  74,  21), S(   9,  18),
    S(  17, -17), S(  52,  -4), S(  35,   0), S(   9,   1),
    S(  29, -81), S(  86, -67), S( -22, -35), S( -16, -36),
};

#undef S

int relativeSquare32(int s, int c) {
    assert(0 <= c && c < COLOUR_NB);
    assert(0 <= s && s < SQUARE_NB);
    static const int edgeDistance[FILE_NB] = {0, 1, 2, 3, 3, 2, 1, 0};
    return 4 * relativeRankOf(c, s) + edgeDistance[fileOf(s)];
}

void initializePSQT() {

    for (int s = 0; s < SQUARE_NB; s++) {
        const int w32 = relativeSquare32(s, WHITE);
        const int b32 = relativeSquare32(s, BLACK);

        PSQT[WHITE_PAWN  ][s] = +MakeScore(PieceValues[PAWN  ][MG], PieceValues[PAWN  ][EG]) +   PawnPSQT32[w32];
        PSQT[WHITE_KNIGHT][s] = +MakeScore(PieceValues[KNIGHT][MG], PieceValues[KNIGHT][EG]) + KnightPSQT32[w32];
        PSQT[WHITE_BISHOP][s] = +MakeScore(PieceValues[BISHOP][MG], PieceValues[BISHOP][EG]) + BishopPSQT32[w32];
        PSQT[WHITE_ROOK  ][s] = +MakeScore(PieceValues[ROOK  ][MG], PieceValues[ROOK  ][EG]) +   RookPSQT32[w32];
        PSQT[WHITE_QUEEN ][s] = +MakeScore(PieceValues[QUEEN ][MG], PieceValues[QUEEN ][EG]) +  QueenPSQT32[w32];
        PSQT[WHITE_KING  ][s] = +MakeScore(PieceValues[KING  ][MG], PieceValues[KING  ][EG]) +   KingPSQT32[w32];

        PSQT[BLACK_PAWN  ][s] = -MakeScore(PieceValues[PAWN  ][MG], PieceValues[PAWN  ][EG]) -   PawnPSQT32[b32];
        PSQT[BLACK_KNIGHT][s] = -MakeScore(PieceValues[KNIGHT][MG], PieceValues[KNIGHT][EG]) - KnightPSQT32[b32];
        PSQT[BLACK_BISHOP][s] = -MakeScore(PieceValues[BISHOP][MG], PieceValues[BISHOP][EG]) - BishopPSQT32[b32];
        PSQT[BLACK_ROOK  ][s] = -MakeScore(PieceValues[ROOK  ][MG], PieceValues[ROOK  ][EG]) -   RookPSQT32[b32];
        PSQT[BLACK_QUEEN ][s] = -MakeScore(PieceValues[QUEEN ][MG], PieceValues[QUEEN ][EG]) -  QueenPSQT32[b32];
        PSQT[BLACK_KING  ][s] = -MakeScore(PieceValues[KING  ][MG], PieceValues[KING  ][EG]) -   KingPSQT32[b32];
    }
}
