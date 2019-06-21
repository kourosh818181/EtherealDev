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

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "movegen.h"

#include "attacks.h"
#include "bitboards.h"
#include "board.h"
#include "masks.h"
#include "move.h"
#include "psqt.h"
#include "search.h"
#include "thread.h"
#include "types.h"
#include "zobrist.h"

static int castleRookFrom(const Board *board, int from, int to) {
    static const uint64_t FirstRank[COLOUR_NB] = {RANK_1, RANK_8};
    const uint64_t rooks = board->castleRooks & FirstRank[board->turn];
    return to > from ? getmsb(rooks) : getlsb(rooks);
}

int apply(Thread *thread, Board *board, uint16_t move, int height) {

    // NULL moves are only tried when legal
    if (move == NULL_MOVE) {
        thread->moveStack[height] = NULL_MOVE;
        applyNullMove(board, &thread->undoStack[height]);
        return 1;
    }

    // Apply the move and reject if illegal
    applyMove(board, move, &thread->undoStack[height]);
    if (!moveWasLegal(board))
        return revertMove(board, move, &thread->undoStack[height]), 0;

    // Keep history on legal moves
    thread->moveStack[height] = move;
    thread->pieceStack[height] = pieceType(board->squares[MoveTo(move)]);
    return 1;
}

void applyMove(Board *board, uint16_t move, Undo *undo) {

    static void (*table[4])(Board*, uint16_t, Undo*) = {
        applyNormalMove, applyCastleMove,
        applyEnpassMove, applyPromotionMove
    };

    uint64_t castleChanges;

    // Save information which is hard to recompute
    undo->hash          = board->hash;
    undo->pkhash        = board->pkhash;
    undo->kingAttackers = board->kingAttackers;
    undo->castleRooks   = board->castleRooks;
    undo->epSquare      = board->epSquare;
    undo->fiftyMoveRule = board->fiftyMoveRule;
    undo->psqtmat       = board->psqtmat;

    // Store hash history for repetition checking
    board->history[board->numMoves++] = board->hash;

    // Run the correct move application function
    table[MoveType(move) >> 12](board, move, undo);

    // No function updated epsquare so we reset
    if (board->epSquare == undo->epSquare) board->epSquare = -1;

    castleChanges = undo->castleRooks ^ board->castleRooks;
    while (castleChanges)
        board->hash ^= ZobristCastleKeys[poplsb(&castleChanges)];

    if (undo->epSquare != -1)
        board->hash ^= ZobristEnpassKeys[fileOf(undo->epSquare)];

    // No function updates this so we do it here
    board->turn = !board->turn;

    // Need king attackers to verify move legality
    board->kingAttackers = attackersToKingSquare(board);
}

void applyNormalMove(Board *board, uint16_t move, Undo *undo) {

    const int from = MoveFrom(move);
    const int to = MoveTo(move);

    const int fromPiece = board->squares[from];
    const int toPiece = board->squares[to];

    const int fromType = pieceType(fromPiece);
    const int toType = pieceType(toPiece);
    const int toColour = pieceColour(toPiece);

    if (fromType == PAWN || toPiece != EMPTY)
        board->fiftyMoveRule = 0;
    else
        board->fiftyMoveRule += 1;

    board->pieces[fromType]     ^= (1ull << from) ^ (1ull << to);
    board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

    board->pieces[toType]    ^= (1ull << to);
    board->colours[toColour] ^= (1ull << to);

    board->squares[from] = EMPTY;
    board->squares[to]   = fromPiece;
    undo->capturePiece   = toPiece;

    board->castleRooks &= board->castleMasks[from];
    board->castleRooks &= board->castleMasks[to];

    board->psqtmat += PSQT[fromPiece][to]
                   -  PSQT[fromPiece][from]
                   -  PSQT[toPiece][to];

    board->hash    ^= ZobristKeys[fromPiece][from]
                   ^  ZobristKeys[fromPiece][to]
                   ^  ZobristKeys[toPiece][to]
                   ^  ZobristTurnKey;

    if (fromType == PAWN || fromType == KING)
        board->pkhash ^= ZobristKeys[fromPiece][from]
                      ^  ZobristKeys[fromPiece][to];

    if (toType == PAWN)
        board->pkhash ^= ZobristKeys[toPiece][to];

    if (fromType == PAWN && (to ^ from) == 16) {

        uint64_t enemyPawns =  board->pieces[PAWN]
                            &  board->colours[!board->turn]
                            &  adjacentFilesMasks(fileOf(from))
                            & (board->turn == WHITE ? RANK_4 : RANK_5);
        if (enemyPawns) {
            board->epSquare = board->turn == WHITE ? from + 8 : from - 8;
            board->hash ^= ZobristEnpassKeys[fileOf(from)];
        }
    }
}

void applyCastleMove(Board *board, uint16_t move, Undo *undo) {

    const int from = MoveFrom(move);
    const int to = MoveTo(move);

    const int rFrom = castleRookFrom(board, from, to);
    const int rTo = to > from ? to - 1 : to + 1;

    const int fromPiece = makePiece(KING, board->turn);
    const int rFromPiece = makePiece(ROOK, board->turn);

    board->fiftyMoveRule += 1;

    board->pieces[KING]         ^= (1ull << from) ^ (1ull << to);
    board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

    board->pieces[ROOK]         ^= (1ull << rFrom) ^ (1ull << rTo);
    board->colours[board->turn] ^= (1ull << rFrom) ^ (1ull << rTo);

    board->squares[from]  = EMPTY;
    board->squares[to]    = fromPiece;

    board->squares[rFrom] = EMPTY;
    board->squares[rTo]   = rFromPiece;

    board->castleRooks &= board->castleMasks[from];

    board->psqtmat += PSQT[fromPiece][to]
                   -  PSQT[fromPiece][from]
                   +  PSQT[rFromPiece][rTo]
                   -  PSQT[rFromPiece][rFrom];

    board->hash    ^= ZobristKeys[fromPiece][from]
                   ^  ZobristKeys[fromPiece][to]
                   ^  ZobristKeys[rFromPiece][rFrom]
                   ^  ZobristKeys[rFromPiece][rTo]
                   ^  ZobristTurnKey;

    board->pkhash  ^= ZobristKeys[fromPiece][from]
                   ^  ZobristKeys[fromPiece][to];

    assert(pieceType(fromPiece) == KING);

    undo->capturePiece = EMPTY;
}

void applyEnpassMove(Board *board, uint16_t move, Undo *undo) {

    const int from = MoveFrom(move);
    const int to = MoveTo(move);
    const int ep = to - 8 + (board->turn << 4);

    const int fromPiece = makePiece(PAWN, board->turn);
    const int enpassPiece = makePiece(PAWN, !board->turn);

    board->fiftyMoveRule = 0;

    board->pieces[PAWN]         ^= (1ull << from) ^ (1ull << to);
    board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

    board->pieces[PAWN]          ^= (1ull << ep);
    board->colours[!board->turn] ^= (1ull << ep);

    board->squares[from] = EMPTY;
    board->squares[to]   = fromPiece;
    board->squares[ep]   = EMPTY;
    undo->capturePiece   = enpassPiece;

    board->psqtmat += PSQT[fromPiece][to]
                   -  PSQT[fromPiece][from]
                   -  PSQT[enpassPiece][ep];

    board->hash    ^= ZobristKeys[fromPiece][from]
                   ^  ZobristKeys[fromPiece][to]
                   ^  ZobristKeys[enpassPiece][ep]
                   ^  ZobristTurnKey;

    board->pkhash  ^= ZobristKeys[fromPiece][from]
                   ^  ZobristKeys[fromPiece][to]
                   ^  ZobristKeys[enpassPiece][ep];

    assert(pieceType(fromPiece) == PAWN);
    assert(pieceType(enpassPiece) == PAWN);
}

void applyPromotionMove(Board *board, uint16_t move, Undo *undo) {

    const int from = MoveFrom(move);
    const int to = MoveTo(move);

    const int fromPiece = board->squares[from];
    const int toPiece = board->squares[to];
    const int promoPiece = makePiece(MovePromoPiece(move), board->turn);

    const int toType = pieceType(toPiece);
    const int toColour = pieceColour(toPiece);
    const int promotype = MovePromoPiece(move);

    board->fiftyMoveRule = 0;

    board->pieces[PAWN]         ^= (1ull << from);
    board->pieces[promotype]    ^= (1ull << to);
    board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

    board->pieces[toType]    ^= (1ull << to);
    board->colours[toColour] ^= (1ull << to);

    board->squares[from] = EMPTY;
    board->squares[to]   = promoPiece;
    undo->capturePiece   = toPiece;

    board->castleRooks &= board->castleMasks[to];

    board->psqtmat += PSQT[promoPiece][to]
                   -  PSQT[fromPiece][from]
                   -  PSQT[toPiece][to];

    board->hash    ^= ZobristKeys[fromPiece][from]
                   ^  ZobristKeys[promoPiece][to]
                   ^  ZobristKeys[toPiece][to]
                   ^  ZobristTurnKey;

    board->pkhash  ^= ZobristKeys[fromPiece][from];

    assert(pieceType(fromPiece) == PAWN);
    assert(pieceType(toPiece) != PAWN);
    assert(pieceType(toPiece) != KING);
}

void applyNullMove(Board *board, Undo *undo) {

    // Save information which is hard to recompute
    // Some information is certain to stay the same
    undo->hash          = board->hash;
    undo->epSquare      = board->epSquare;
    undo->fiftyMoveRule = board->fiftyMoveRule++;

    // NULL moves simply swap the turn only
    board->turn = !board->turn;
    board->history[board->numMoves++] = board->hash;

    // Update the hash for turn and changes to enpass square
    board->hash ^= ZobristTurnKey;
    if (board->epSquare != -1) {
        board->hash ^= ZobristEnpassKeys[fileOf(board->epSquare)];
        board->epSquare = -1;
    }
}

void revert(Thread *thread, Board *board, uint16_t move, int height) {
    if (move == NULL_MOVE) revertNullMove(board, &thread->undoStack[height]);
    else revertMove(board, move, &thread->undoStack[height]);
}

void revertMove(Board *board, uint16_t move, Undo *undo) {

    const int to = MoveTo(move);
    const int from = MoveFrom(move);

    // Revert information which is hard to recompute
    board->hash          = undo->hash;
    board->pkhash        = undo->pkhash;
    board->kingAttackers = undo->kingAttackers;
    board->castleRooks   = undo->castleRooks;
    board->epSquare      = undo->epSquare;
    board->fiftyMoveRule = undo->fiftyMoveRule;
    board->psqtmat       = undo->psqtmat;

    // Swap turns and update the history index
    board->turn = !board->turn;
    board->numMoves--;

    if (MoveType(move) == NORMAL_MOVE) {

        const int fromType = pieceType(board->squares[to]);
        const int toType = pieceType(undo->capturePiece);
        const int toColour = pieceColour(undo->capturePiece);

        board->pieces[fromType]     ^= (1ull << from) ^ (1ull << to);
        board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

        board->pieces[toType]    ^= (1ull << to);
        board->colours[toColour] ^= (1ull << to);

        board->squares[from] = board->squares[to];
        board->squares[to] = undo->capturePiece;
    }

    else if (MoveType(move) == CASTLE_MOVE) {

        const int rFrom = castleRookFrom(board, from, to);
        const int rTo = to > from ? to - 1 : to + 1;

        board->pieces[KING]         ^= (1ull << from) ^ (1ull << to);
        board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

        board->pieces[ROOK]         ^= (1ull << rFrom) ^ (1ull << rTo);
        board->colours[board->turn] ^= (1ull << rFrom) ^ (1ull << rTo);

        board->squares[from] = board->squares[to];
        board->squares[to] = EMPTY;

        board->squares[rFrom] = board->squares[rTo];
        board->squares[rTo] = EMPTY;
    }

    else if (MoveType(move) == PROMOTION_MOVE) {

        const int toType = pieceType(undo->capturePiece);
        const int toColour = pieceColour(undo->capturePiece);
        const int promotype = MovePromoPiece(move);

        board->pieces[PAWN]         ^= (1ull << from);
        board->pieces[promotype]    ^= (1ull << to);
        board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

        board->pieces[toType]    ^= (1ull << to);
        board->colours[toColour] ^= (1ull << to);

        board->squares[from] = makePiece(PAWN, board->turn);
        board->squares[to] = undo->capturePiece;
    }

    else { // (MoveType(move) == ENPASS_MOVE)

        assert(MoveType(move) == ENPASS_MOVE);

        const int ep = to - 8 + (board->turn << 4);

        board->pieces[PAWN]         ^= (1ull << from) ^ (1ull << to);
        board->colours[board->turn] ^= (1ull << from) ^ (1ull << to);

        board->pieces[PAWN]          ^= (1ull << ep);
        board->colours[!board->turn] ^= (1ull << ep);

        board->squares[from] = board->squares[to];
        board->squares[to] = EMPTY;
        board->squares[ep] = undo->capturePiece;
    }
}

void revertNullMove(Board *board, Undo *undo) {

    // Revert information which is hard to recompute
    // We may, and have to, zero out the king attacks
    board->hash          = undo->hash;
    board->kingAttackers = 0ull;
    board->epSquare      = undo->epSquare;
    board->fiftyMoveRule = undo->fiftyMoveRule;

    // NULL moves simply swap the turn only
    board->turn = !board->turn;
    board->numMoves--;
}

int moveIsTactical(Board *board, uint16_t move) {

    // We can use a simple bit trick since we assert that only
    // the enpass and promotion moves will ever have the 13th bit,
    // (ie 2 << 12) set. We use (2 << 12) in order to match move.h
    assert(ENPASS_MOVE & PROMOTION_MOVE & (2 << 12));
    assert(!((NORMAL_MOVE | CASTLE_MOVE) & (2 << 12)));

    // Check for captures, promotions, or enpassant
    return board->squares[MoveTo(move)] != EMPTY
        || (move & ENPASS_MOVE & PROMOTION_MOVE);
}

int moveEstimatedValue(Board *board, uint16_t move) {

    // Start with the value of the piece on the target square
    int value = SEEPieceValues[pieceType(board->squares[MoveTo(move)])];

    // Factor in the new piece's value and remove our promoted pawn
    if (MoveType(move) == PROMOTION_MOVE)
        value += SEEPieceValues[MovePromoPiece(move)] - SEEPieceValues[PAWN];

    // Target square is encoded as empty for enpass moves
    if (MoveType(move) == ENPASS_MOVE)
        value = SEEPieceValues[PAWN];

    return value;
}

int moveBestCaseValue(Board *board) {

    // Assume the opponent has at least a pawn
    int value = SEEPieceValues[PAWN];

    // Check for a higher value target
    for (int piece = QUEEN; piece > PAWN; piece--)
        if (board->pieces[piece] & board->colours[!board->turn])
          { value = SEEPieceValues[piece]; break; }

    // Check for a potential pawn promotion
    if (   board->pieces[PAWN]
        &  board->colours[board->turn]
        & (board->turn == WHITE ? RANK_7 : RANK_2))
        value += SEEPieceValues[QUEEN] - SEEPieceValues[PAWN];

    return value;
}

int moveWasLegal(Board *board) {

    // Grab the last player's King's square and verify safety
    int sq = getlsb(board->colours[!board->turn] & board->pieces[KING]);
    assert(board->squares[sq] == makePiece(KING, !board->turn));
    return !squareIsAttacked(board, !board->turn, sq);
}

int moveIsPsuedoLegal(Board *board, uint16_t move) {

    int from   = MoveFrom(move);
    int type   = MoveType(move);
    int ftype  = pieceType(board->squares[from]);
    int rook, king, rookTo, kingTo;

    uint64_t friendly = board->colours[ board->turn];
    uint64_t enemy    = board->colours[!board->turn];
    uint64_t castles  = friendly & board->castleRooks;
    uint64_t occupied = friendly | enemy;
    uint64_t attacks, forward, mask;

    // Quick check against obvious illegal moves, moving from an empty
    // or enemy square, and moves with invalid promotion flags enabled
    if (   (move == NONE_MOVE || move == NULL_MOVE)
        || (pieceColour(board->squares[from]) != board->turn)
        || (MovePromoType(move) != PROMOTE_TO_KNIGHT && type != PROMOTION_MOVE))
        return 0;

    // Knight, Bishop, Rook, and Queen moves are legal so long as the
    // move type is NORMAL and the destination is an attacked square

    if (ftype == KNIGHT)
        return type == NORMAL_MOVE
            && testBit(knightAttacks(from) & ~friendly, MoveTo(move));

    if (ftype == BISHOP)
        return type == NORMAL_MOVE
            && testBit(bishopAttacks(from, occupied) & ~friendly, MoveTo(move));

    if (ftype == ROOK)
        return type == NORMAL_MOVE
            && testBit(rookAttacks(from, occupied) & ~friendly, MoveTo(move));

    if (ftype == QUEEN)
        return type == NORMAL_MOVE
            && testBit(queenAttacks(from, occupied) & ~friendly, MoveTo(move));

    if (ftype == PAWN) {

        // Throw out castle moves with our pawn
        if (type == CASTLE_MOVE)
            return 0;

        // Look at the squares which our pawn threatens
        attacks = pawnAttacks(board->turn, from);

        // Enpass moves are legal if our to square is the enpass
        // square and we could attack a piece on the enpass square
        if (type == ENPASS_MOVE)
            return MoveTo(move) == board->epSquare && testBit(attacks, MoveTo(move));

        // Compute simple pawn advances
        forward = pawnAdvance(1ull << from, occupied, board->turn);

        // Promotion moves are legal if we can move to one of the promotion
        // ranks, defined by PROMOTION_RANKS, independent of moving colour
        if (type == PROMOTION_MOVE)
            return testBit(PROMOTION_RANKS & ((attacks & enemy) | forward), MoveTo(move));

        // Add the double advance to forward
        forward |= pawnAdvance(forward & (!board->turn ? RANK_3 : RANK_6), occupied, board->turn);

        // Normal moves are legal if we can move there
        return testBit(~PROMOTION_RANKS & ((attacks & enemy) | forward), MoveTo(move));
    }

    // The colour check should (assuming board->squares only contains
    // pieces and EMPTY flags) ensure that ftype is an actual piece,
    // which at this point the only piece left to check is the King
    assert(ftype == KING);

    // Normal moves are legal if the to square is a valid target
    if (type == NORMAL_MOVE)
        return testBit(kingAttacks(from) & ~friendly, MoveTo(move));

    // Kings cannot enpass, promote, or castle out of check
    if (type != CASTLE_MOVE || board->kingAttackers)
        return 0;

    // Verifying a castle move can be difficult, so instead we will just
    // attempt to generate the (two) possible castle moves for the given
    // player. If one matches, we can then verify the psuedo legality
    // using the same code as from movegen.c


    int _size = 0;
    uint16_t _moves[MAX_MOVES];
    genAllQuietMoves(board, _moves, &_size);

    for (int i = 0; i < _size; i++)
        if (move == _moves[i])
            return 1;
    return 0;



    while (castles) {

        // Figure out which pieces are moving to which squares
        rook = poplsb(&castles), king = from;
        kingTo = square(rankOf(rook), rook > king ? 6 : 2);
        rookTo = rook > king ? kingTo - 1 : kingTo + 1;

        // Make sure we are generating the
        if (move != MoveMake(king, kingTo, CASTLE_MOVE))
            continue;

        // Castle is illegal if we would go over a piece
        mask  = bitsBetweenMasks(king, kingTo);
        mask |= bitsBetweenMasks(rook, rookTo);
        mask &= ~((1ull << king) | (1ull << rook));
        if (occupied & mask) return 0;

        // Castle is illegal if we move through a checking threat
        mask = bitsBetweenMasks(king, kingTo);
        while (mask)
            if (squareIsAttacked(board, board->turn, poplsb(&mask)))
                return 0;

        return 1; // All requirments are met
    }

    return 0;
}

void moveToString(uint16_t move, char *str) {

    // Encode squares (Long Algebraic Notation)
    squareToString(MoveFrom(move), &str[0]);
    squareToString(MoveTo(move), &str[2]);

    // Add promotion piece label (Uppercase)
    if (MoveType(move) == PROMOTION_MOVE) {
        str[4] = PieceLabel[BLACK][MovePromoPiece(move)];
        str[5] = '\0';
    }
}
