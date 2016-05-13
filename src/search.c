#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <time.h>

#include "bitboards.h"
#include "bitutils.h"
#include "board.h"
#include "castle.h"
#include "evaluate.h"
#include "magics.h"
#include "piece.h"
#include "search.h"
#include "transposition.h"
#include "types.h"
#include "move.h"
#include "movegen.h"
#include "movegentest.h"
#include "zorbist.h"

time_t StartTime;
time_t EndTime;

int TotalNodes;

int EvaluatingPlayer;
int MaxReached;

uint16_t KillerMoves[MaxHeight][2];
uint16_t KillerCaptures[MaxHeight][2];

TransTable Table;

uint16_t getBestMove(Board * board, int seconds, int logging){
    
    // INITALIZE SEARCH GLOBALS
    StartTime = time(NULL);
    EndTime = StartTime + seconds;
    TotalNodes = 0;
    EvaluatingPlayer = board->turn;
    initalizeTranspositionTable(&Table, 22);
    
    // POPULATE ROOT'S MOVELIST
    MoveList rootMoveList;
    rootMoveList.size = 0;
    genAllMoves(board,rootMoveList.moves,&(rootMoveList.size));
    
    // PRINT SEARCH DATA TABLE HEADER TO CONSOLE
    if (!logging){
        printBoard(board);
        printf("|  Depth  |  Score  |   Nodes   | Elapsed | Best |\n");
    }
    
    int depth, value;
    for (depth = 1; depth < MaxDepth; depth++){
        
        MaxReached = 0;
        
        // PERFORM FULL SEARCH ON ROOT
        value = rootSearch(board,&rootMoveList,depth);
        
        // LOG RESULTS TO INTERFACE
        if (logging){
            printf("info depth %d score cp %d time %d nodes %d pv ",depth,(100*value)/PawnValue,1000*(time(NULL)-StartTime),TotalNodes);
            printMove(rootMoveList.bestMove);
            printf("\n");
            fflush(stdout);
        }
        
        // LOG RESULTS TO CONSOLE
        else {
            printf("|%9d|%9d|%11d|%9d| ",depth,(100*value)/PawnValue,TotalNodes,(time(NULL)-StartTime));
            printMove(rootMoveList.bestMove);
            printf(" | %d\n",MaxReached);
        }
        
        // END THE SEARCH IF THE NEXT DEPTH IS EXPECTED
        // TO TAKE LONGER THAN THE TOTAL ALLOTED TIME
        if ((time(NULL) - StartTime) * 3 >= seconds)
            break;
    }
    
    // PRINT TABLE DATA TO CONSOLE
    if (!logging)
        dumpTranspositionTable(&Table);
    
    return rootMoveList.bestMove;    
}

int rootSearch(Board * board, MoveList * moveList, int depth){
    
    int alpha = -Mate, beta = Mate;
    int i, best =-Mate, value;
    int currentNodes;
    Undo undo[1];
    
    assert(board != NULL);
    assert(moveList != NULL);
    assert(moveList->size != 0);
    assert(depth != 0);    
   
    for (i = 0; i < moveList->size; i++){
        
        currentNodes = TotalNodes;
        
        // APPLY MOVE
        applyMove(board, moveList->moves[i], undo);
        
        // FULL WINDOW SEARCH ON FIRST MOVE
        if (i == 0)
            value = -alphaBetaSearch(board, -beta, -alpha, depth-1, 1, PVNODE);
        
        // NULL WINDOW SEARCH ON NON-FIRST MOVES
        else{
            value = -alphaBetaSearch(board, -alpha-1, -alpha, depth-1, 1, CUTNODE);
            
            // NULL WINDOW FAILED HIGH, RESEARCH
            if (value > alpha)
                value = -alphaBetaSearch(board, -beta, -alpha, depth-1, 1, PVNODE);
        }
        
        // REVERT MOVE FROM BOARD
        revertMove(board, moveList->moves[i], undo);
        
        if (value <= alpha)
            moveList->values[i] = -(1<<28) + (TotalNodes - currentNodes); // UPPER VALUE
        else if (value >= beta)
            moveList->values[i] = beta;  // LOWER VALUE
        else
            moveList->values[i] = value; // EXACT VALUE
        
        
        // IMPROVED CURRENT VALUE
        if (value > best){
            best = value;
            moveList->bestMove = moveList->moves[i];
            
            // IMPROVED CURRENT LOWER VALUE
            if (value > alpha)
                alpha = value;
        }
        
        // IMPROVED AND FAILED HIGH
        if (alpha >= beta)
            break;
    }
    
    // SORT MOVELIST FOR NEXT ITERATION
    sortMoveList(moveList);
    return best;
}

int alphaBetaSearch(Board * board, int alpha, int beta, int depth, int height, int nodeType){
    
    // INITIALIZE SEARCH VARIABLES
    int i, value, newDepth, tempDepth, inCheck, entryValue, entryType, values[256];
    int size = 0, best = -Mate, repeated = 0, usedTableEntry = 0, hasFailedHigh = 0;
    int oldAlpha = alpha, optimalValue = -Mate;
    uint16_t currentMove, moves[256], tableMove = NoneMove, bestMove = NoneMove;
    TransEntry * entry;
    Undo undo[1];
    
    // VALIDATE PARAMETERS
    assert(board != NULL);
    assert(alpha >= -Mate);
    assert(beta <= Mate);
    assert(height != 0);
    assert(nodeType <= 3 && nodeType >= 1);
    
    // SEARCH TIME HAS EXPIRED
    if (EndTime < time(NULL))
        return board->turn == EvaluatingPlayer ? -Mate : Mate;
    
    // SEARCH HORIZON REACHED
    if (depth <= 0)
        return quiescenceSearch(board, alpha, beta, height);
    
    // SEARCH HAS REACHED MAX HEIGHT
    if (height >= MaxHeight)
        return evaluate_board(board);
    
    // UPDATE SEARCH INFO
    TotalNodes++;
    MaxReached = MaxReached > height ? MaxReached : height;
    
    // LOOKUP CURRENT POSITION IN TRANSPOSITION TABLE
    entry = getTranspositionEntry(&Table, board->hash);
    
    if (entry != NULL
        && board->turn == EntryTurn(*entry)){
        
        // ENTRY MOVE MAY BE CANDIDATE
        tableMove = EntryMove(*entry);
        
        // ENTRY MAY IMPROVE BOUNDS
        if (USE_TRANSPOSITION_TABLE
            && EntryDepth(*entry) >= depth
            && nodeType != PVNODE){
                
            entryValue = EntryValue(*entry);
            entryType = EntryType(*entry);
            
            // EXACT VALUE STORED
            if (entryType == PVNODE)
                return entryValue;
            
            // LOWER BOUND STORED
            else if (entryType == CUTNODE)
                alpha = entryValue > alpha ? entryValue : alpha;
            
            // UPPER BOUND STORED
            else if (entryType == ALLNODE)
                beta = entryValue < best ? entryValue : beta;
            
            // BOUNDS NOW OVERLAP?
            if (alpha >= beta)
                return entryValue;
            
            usedTableEntry = 1;
            oldAlpha = alpha;
        }
    }
    
    // DETERMINE 3-FOLD REPITION
    for (i = board->numMoves-2; i >= 0; i-=2)
        if (board->history[i] == board->hash)
            repeated++;
    if (repeated >= 2)
        return 0;
    
    // DETERMINE CHECK STATUS
    inCheck = !isNotInCheck(board, board->turn);
    
    // RAZOR PRUNING
    if (USE_RAZOR_PRUNING
        && !inCheck
        && depth <= 3
        && nodeType != PVNODE
        && alpha == beta - 1
        && evaluate_board(board) + KnightValue < beta){
        
        value = quiescenceSearch(board, alpha, beta, height);
        
        // EVEN GAINING A KNIGHT WOULD FAIL LOW
        if (value < beta)
            return value;
    }
    
    // USE NULL MOVE PRUNING
    if (USE_NULL_MOVE_PRUNING
        && depth >= 3 
        && abs(beta) < Mate - MaxHeight
        && alpha == beta - 1
        && nodeType != PVNODE
        && canDoNull(board)
        && !inCheck
        && evaluate_board(board) >= beta){
            
        // APPLY NULL MOVE
        board->turn = !board->turn;
        board->history[board->numMoves++] = NullMove;
        
        // PERFORM NULL MOVE SEARCH
        value = -alphaBetaSearch(board, -beta, -beta+1, depth-4, height+1, CUTNODE);
        
        // REVERT NULL MOVE
        board->numMoves--;
        board->turn = !board->turn;
        
        if (value >= beta)
            return value;
    }
    
    // INTERNAL ITERATIVE DEEPING
    if (USE_INTERNAL_ITERATIVE_DEEPENING
        && depth >= 3
        && tableMove == NoneMove
        && nodeType == PVNODE){
        
        // SEARCH AT A LOWER DEPTH
        value = alphaBetaSearch(board, alpha, beta, depth-3, height, PVNODE);
        if (value <= alpha)
            value = alphaBetaSearch(board, -Mate, beta, depth-3, height, PVNODE);
        
        // GET TABLE MOVE FROM TRANSPOSITION TABLE
        entry = getTranspositionEntry(&Table, board->hash);
        if (entry != NULL)
            tableMove = EntryMove(*entry);
    }
    
    // GENERATE AND PREPARE MOVE ORDERING
    genAllMoves(board, moves, &size);
    evaluateMoves(board, values, moves, size, height, tableMove);
    
    // DETERMINE IF EXTENSIONS WILL BE USED
    tempDepth = (size == 1 || inCheck) ? depth : depth-1;
    
    for (i = 0; i < size; i++){
        
        // GET NEXT MOVE FROM MOVE LIST
        currentMove = getNextMove(moves, values, i, size);
        
        // USE FUTILITY PRUNING
        if (USE_FUTILITY_PRUNING
            && nodeType != PVNODE
            && depth <= 1
            && MoveType(currentMove) == NormalMove
            && board->squares[MoveTo(currentMove)] == Empty){
         
            if (optimalValue == -Mate)
                optimalValue = evaluate_board(board) + PawnValue;
                
            value = optimalValue;
            
            if (value <= alpha)
                continue;
        }
        
        // APPLY MOVE
        applyMove(board, currentMove, undo);
        
        // SET NEW DEPTH BEFORE LMR
        newDepth = tempDepth;
        
        // DETERMINE IF WE CAN USE LATE MOVE REDUCTIONS
        if (USE_LATE_MOVE_REDUCTIONS
            && !inCheck
            && i >= 3
            && depth >= 3
            && nodeType != PVNODE
            && (
                   (MoveType(currentMove) == NormalMove
                    && board->squares[MoveTo(currentMove)] >= KnightFlag)
                || (MoveType(currentMove) == PromotionMove
                    && (currentMove & PromoteToQueen) == 0)
            )
            && undo[0].capturePiece == Empty
            && isNotInCheck(board, board->turn)){
                
            if (i >= 8){
                if (bestMove == tableMove
                    && !hasFailedHigh)
                    newDepth = tempDepth - 3;
                else
                    newDepth = tempDepth - 2;
            }
            else
                newDepth = tempDepth - 1;
        }
        
        // FULL WINDOW SEARCH ON FIRST MOVE
        if (i == 0 || nodeType != PVNODE){
            value = -alphaBetaSearch(board, -beta, -alpha, newDepth, height+1, nodeType);
            
            // IMPROVED BOUND, BUT WAS REDUCED DEPTH?
            if (value > alpha
                && newDepth != tempDepth){
                
                hasFailedHigh = 1;
                value = -alphaBetaSearch(board, -beta, -alpha, tempDepth, height+1, nodeType);
            }
        }
        
        // NULL WINDOW SEARCH ON NON-FIRST / PV MOVES
        else{
            value = -alphaBetaSearch(board, -alpha-1, -alpha, newDepth, height+1, CUTNODE);
            
            // NULL WINDOW FAILED HIGH, RESEARCH
            if (value > alpha){
                hasFailedHigh = 1;
                value = -alphaBetaSearch(board, -beta, -alpha, tempDepth, height+1, PVNODE);
            }
        }
        
        // REVERT MOVE
        revertMove(board, currentMove, undo);
        
        // IMPROVED CURRENT VALUE
        if (value > best){
            best = value;
            bestMove = currentMove;
            
            // IMPROVED CURRENT LOWER VALUE
            if (value > alpha)
                alpha = value;
        }
        
        // IMPROVED AND FAILED HIGH
        if (alpha >= beta){
            
            // UPDATE QUIET-KILLER MOVES
            if (MoveType(currentMove) == CastleMove
                || MoveType(currentMove) == EnpassMove
                || (MoveType(currentMove) == NormalMove
                    && undo[0].capturePiece == Empty)){
                KillerMoves[height][1] = KillerMoves[height][0];
                KillerMoves[height][0] = currentMove;
            }
            
            // UPDATE NOISY-KILLER MOVES
            else {
                KillerCaptures[height][1] = KillerCaptures[height][0];
                KillerCaptures[height][0] = currentMove;
            }
            
            break;
        }
    }
    
    // BOARD IS STALEMATE OR CHECKMATE
    if (size == 0){
        
        // BOARD IS STALEMATE
        if (isNotInCheck(board, board->turn))
            return 0;
        
        // BOARD IS CHECKMATE
        else
            return -Mate+height;
    }
    
    // STORE RESULTS IN TRANSPOSITION TABLE
    if (best > oldAlpha && best < beta)
        storeTranspositionEntry(&Table, tempDepth + 1, board->turn,  PVNODE, best, bestMove, board->hash);
    else if (best >= beta)
        storeTranspositionEntry(&Table, tempDepth + 1, board->turn, CUTNODE, best, bestMove, board->hash);
    else if (best <= oldAlpha)
        storeTranspositionEntry(&Table, tempDepth + 1, board->turn, ALLNODE, best, bestMove, board->hash);
    
    return best;
}

int quiescenceSearch(Board * board, int alpha, int beta, int height){
    int i, size = 0, value, best = -2*Mate, values[256];
    uint16_t moves[256], bestMove, currentMove;
    Undo undo[1];
    
    // MAX HEIGHT REACHED, STOP HERE
    if (height >= MaxHeight)
        return evaluate_board(board);
    
    // UPDATE SEARCH INFO
    TotalNodes++;
    MaxReached = MaxReached > height ? MaxReached : height;
    
    // GET A STANDING-EVAL OF THE CURRENT BOARD
    value = evaluate_board(board);
    
    // UPDATE LOWER BOUND
    if (value > alpha)
        alpha = value;
    
    // BOUNDS NOW OVERLAP?
    if (alpha >= beta)
        return value;
    
    // DELTA PRUNING IN WHEN NO PROMOTIONS AND NOT EXTREME LATE GAME
    if (value + QueenValue < alpha
        && board->numPieces >= 5
        && !(board->colourBitBoards[0] & board->pieceBitBoards[0] & RANK_7)
        && !(board->colourBitBoards[1] & board->pieceBitBoards[0] & RANK_2)
        && isNotInCheck(board,board->turn))
        return alpha;
    
    // GENERATE AND PREPARE QUIET MOVE ORDERING
    genAllNonQuiet(board, moves, &size);
    evaluateMoves(board, values, moves, size, height, NoneMove);
    
    best = value;
    
    for (i = 0; i < size; i++){
        
        // GET NEXT MOVE FROM MOVE LIST
        currentMove = getNextMove(moves, values, i, size);
        
        // APPLY MOVE
        applyMove(board, currentMove, undo);
        
        // SEARCH NEXT DEPTH
        value = -quiescenceSearch(board, -beta, -alpha, height+1);
        
        // REVERT MOVE
        revertMove(board, currentMove, undo);
        
        // IMPROVED CURRENT VALUE
        if (value > best){
            best = value;
            
            // IMPROVED CURRENT LOWER VALUE
            if (value > alpha)
                alpha = value;
        }
        
        // IMPROVED AND FAILED HIGH
        if (alpha >= beta){
            
            // UPDATE NOISY-KILLER MOVES
            KillerCaptures[height][1] = KillerCaptures[height][0];
            KillerCaptures[height][0] = currentMove;
            
            break;
        }
    }
    
    return best;
}

void evaluateMoves(Board * board, int * values, uint16_t * moves, int size, int height, uint16_t tableMove){
    int i, value;
    int from_type, to_type;
    int from_val, to_val;
    
    // GET KILLER MOVES
    uint16_t killer1 = KillerMoves[height][0];
    uint16_t killer2 = KillerMoves[height][1];
    uint16_t killer3 = KillerCaptures[height][0];
    uint16_t killer4 = KillerCaptures[height][1];
    
    
    for (i = 0; i < size; i++){
        
        // TABLEMOVE FIRST
        value  = 16384 * ( tableMove == moves[i]);
        
        // THEN KILLERS, UNLESS OTHER GOOD CAPTURE
        value += 256   * (   killer1 == moves[i]);
        value += 256   * (   killer2 == moves[i]);
        value +=  32   * (   killer3 == moves[i]);
        value +=  32   * (   killer4 == moves[i]);
        
        // INFO FOR POSSIBLE CAPTURE
        from_type = PieceType(board->squares[MoveFrom(moves[i])]);
        to_type = PieceType(board->squares[MoveTo(moves[i])]);
        from_val = PieceValues[from_type];
        to_val = PieceValues[to_type];
        
        // ENCOURAGE CAPTURING HIGH VALUE WITH LOW VALUE
        value += 5 * to_val;
        value -= 1 * from_val;
        
        // ENPASS CAPTURE IS TREATED SEPERATLY
        if (MoveType(moves[i]) == EnpassMove)
            value += 2*PawnValue;
        
        // WE ARE ONLY CONCERED WITH QUEEN PROMOTIONS
        if (MoveType(moves[i]) == PromotionMove)
            value += QueenValue * (moves[i] & PromoteToQueen);
        
        // ENCOURAGE CASTLING
        if (MoveType(moves[i]) == CastleMove)
            value += KingValue;
        
        values[i] = value;
    }
}

uint16_t getNextMove(uint16_t * moves, int * values, int index, int size){
    int i, best = 0;
    uint16_t bestMove;
    
    // FIND GREATEST VALUE
    for (i = 1; i < size-index; i++)
        if (values[i] > values[best])
            best = i;
        
    bestMove = moves[best];
    
    // MOVE LAST PAIR TO BEST SO WE
    // CAN REDUCE THE EFFECTIVE LIST SIZE
    moves[best] = moves[size-index-1];
    values[best] = values[size-index-1];
    
    return bestMove;
}

void sortMoveList(MoveList * moveList){
    int i, j, tempVal;
    uint16_t tempMove;
    
    for (i = 0; i < moveList->size; i++){
        for (j = i+1; j < moveList->size; j++){
            if (moveList->values[j] > moveList->values[i]){
                
                tempVal = moveList->values[j];
                tempMove = moveList->moves[j];
                
                moveList->values[j] = moveList->values[i];
                moveList->moves[j] = moveList->moves[i];
                
                moveList->values[i] = tempVal;
                moveList->moves[i] = tempMove;
            }
        }
    }    
}

int canDoNull(Board * board){
    uint64_t friendly = board->colourBitBoards[board->turn];
    uint64_t kings = board->pieceBitBoards[5];
    
    return (friendly & kings) != friendly;
}