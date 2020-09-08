/* Chess Loadable Kernel Module
File:		chess.c
Author:		Sasha Arsenyuk
Course:		CMSC 421, Section 2
Term:		Spring 2020
Instructor:	Lawrence Sebald
*/

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/slab.h>	/* for kmalloc() */
#include <linux/mutex.h>

MODULE_LICENSE("GPL");

#define DEVICE_NAME	"chess"
#define MAX_MINOR	1

/* Piece types */
#define	PAWN	0
#define	ROOK	8
#define	KNIGHT	10
#define	BISHOP	12
#define	QUEEN	14
#define	KING	15

typedef struct coord_t coord_t;
typedef struct piece_t piece_t;

/* Prototypes for device functions */
static ssize_t	d_read(struct file *, char __user *, size_t, loff_t *);
static ssize_t	d_write(struct file *, const char __user *, size_t, loff_t *);
static int	d_open(struct inode *, struct file *);
static int	d_release(struct inode *, struct file *);

/* Helper function prototypes */
static void set_board(int);
static void display_board(int);
static int coord_to_sq(coord_t);
static coord_t sq_to_coord(int);

/* Choose a random legal move for the CPU */
static int make_move(int, char, int);
/* Fills an array with all legal moves for a given piece */
static int find_move(int, coord_t*, piece_t);
static int pawn_helper(int, coord_t*, int, piece_t);
static int verify_move_helper(int, coord_t, char);
static int knight_helper(int, coord_t*, int, piece_t);
static int rook_helper(int, coord_t*, int, piece_t);
static int bishop_helper(int, coord_t*, int, piece_t);
static int queen_helper(int, coord_t*, int, piece_t);
static int king_helper(int, coord_t*, int, piece_t);

/* Verify that player made a valid move */
static int move_valid(int, piece_t, coord_t, int, int, piece_t, piece_t);

static int in_check(int, char);

/* This structure holds the addresses of functions
*  that perform device operations.*/
static const struct file_operations fops = {
	.owner	= THIS_MODULE,
	.read	= d_read,
	.write	= d_write,
	.open	= d_open,
	.release = d_release
};

struct coord_t {
	int x; /* x - row (letters), y - column (numbers);
		both 0 through 7 */
	int y;
};

struct piece_t {
	int type; /* 0 - pawn, 8 - rook, 10 - knight
		12 - bishop, 14 - queen, 15 - king */
	char color; /* W/B */
	int alive; /* 0 - dead, 1 - alive */
	coord_t square; /* coordinate of form (x, y)
			E4 would be (4, 3) */
};

struct d_data {
	struct cdev cdev;
	int game_on;	/* Is a game in progress? */
	int board[64];	/* Stores indexes of the figure array
			or -1 if the square is empty */
	piece_t figures[32]; /* Store the information about the pieces
				first 16 are white, other 16 are black
				first 8 are pawns, then 2 rooks,
				2 knights, 2 bishops, a queen, and a king.*/
	char turn;	/* W/B */
	char player_color;
	char computer_color;
	char reply[130];	/* Store the most recent reply */
};

/* Global variables */
static int major = 0;
static struct d_data cdev_data[MAX_MINOR];
static struct class *cdev_class = NULL;
static DEFINE_MUTEX(d_mutex);

static int cdev_uevent(struct device *dev, struct kobj_uevent_env *env) {
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

/* Helper procedures to convert between coordinates and squares */
static int coord_to_sq(coord_t c) {
	if (c.x < 0 || c.x > 7 || c.y < 0 || c.y > 7) {
		return -1;
	}
	return (8 * c.y + c.x);
}

static coord_t sq_to_coord(int s) {
	coord_t c;
	if (s < 0 || s > 64) {
		c.x = -1;
		c.y = -1;
		return c;
	}
	c.y = s / 8;
	c.x = s % 8;
	return c;
}

/* Display current state of the board */
static void display_board(int d_num) {
	mutex_lock(&d_mutex);
	int i;
	for (i = 0; i < 128; i = i + 2) {
		int piece_index;
		piece_index = cdev_data[d_num].board[i / 2];

		/* Empty square */
		if (piece_index == -1) {
			cdev_data[d_num].reply[i] = '*';
			cdev_data[d_num].reply[i + 1] = '*';
		}
		/* An occupied square */
		else {
			/* Determine piece color */
			if (cdev_data[d_num].figures[piece_index].color == 'W') {
				cdev_data[d_num].reply[i] = 'W';
			}
			else {
				cdev_data[d_num].reply[i] = 'B';
			}

			if (cdev_data[d_num].figures[piece_index].type == PAWN) {
				cdev_data[d_num].reply[i + 1] = 'P';
			}
			else if (cdev_data[d_num].figures[piece_index].type == ROOK) {
				cdev_data[d_num].reply[i + 1] = 'R';
			}
			else if (cdev_data[d_num].figures[piece_index].type == KNIGHT) {
				cdev_data[d_num].reply[i + 1] = 'N';
			}
			else if (cdev_data[d_num].figures[piece_index].type == BISHOP) {
				cdev_data[d_num].reply[i + 1] = 'B';
			}
			else if (cdev_data[d_num].figures[piece_index].type == QUEEN) {
				cdev_data[d_num].reply[i + 1] = 'Q';
			}
			else {
				cdev_data[d_num].reply[i + 1] = 'K';
			}
		}
	}
	cdev_data[d_num].reply[i++] = '\n';
	cdev_data[d_num].reply[i] = '\0';

	mutex_unlock(&d_mutex);
}

/* Perform initial board set-up */
static void set_board(int d_num) {
	mutex_lock(&d_mutex);

	cdev_data[d_num].game_on = 1;
	cdev_data[d_num].turn = 'W';
	/* Set up board */
	int i;
	/* Fill in the pawns */
	for (i = 0; i < 8; ++i) {
		/* White */
		cdev_data[d_num].figures[i].type = PAWN;
		cdev_data[d_num].figures[i].color = 'W';
		cdev_data[d_num].figures[i].alive = 1;
		cdev_data[d_num].figures[i].square.x = i;
		cdev_data[d_num].figures[i].square.y = 1;

		int square;
		square = coord_to_sq(cdev_data[d_num].figures[i].square);
		cdev_data[d_num].board[square] = i;

		/* Black */
		int j;
		j = i + 16; /* Offset in the array */
		cdev_data[d_num].figures[j].type = PAWN;
		cdev_data[d_num].figures[j].color = 'B';
		cdev_data[d_num].figures[j].alive = 1;
		cdev_data[d_num].figures[j].square.x = i;
		cdev_data[d_num].figures[j].square.y = 6;

		square = coord_to_sq(cdev_data[d_num].figures[j].square);
		cdev_data[d_num].board[square] = j;
	}

	/* Fill in the rooks */
	int w = 8;
	int square_w = 0;

	cdev_data[d_num].figures[w].type = ROOK;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 0;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w] = w;

	int b = i + 16;
	int square_b = square_w + 56;

	cdev_data[d_num].figures[b].type = ROOK;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 0;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b] = b;

	++w;
	++b;

	cdev_data[d_num].figures[w].type = ROOK;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 7;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w + 7] = w;

	cdev_data[d_num].figures[b].type = ROOK;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 7;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b + 7] = b;


	/* Fill in the knights */
	++w;
	++b;
	++square_w;
	++square_b;

	cdev_data[d_num].figures[w].type = KNIGHT;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 1;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w] = w;

	cdev_data[d_num].figures[b].type = KNIGHT;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 1;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b] = b;

	++w;
	++b;

	cdev_data[d_num].figures[w].type = KNIGHT;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 6;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w + 5] = w;

	cdev_data[d_num].figures[b].type = KNIGHT;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 6;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b + 5] = b;

	/* Fill in the bishops */
	++w;
	++b;
	++square_w;
	++square_b;

	cdev_data[d_num].figures[w].type = BISHOP;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 2;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w] = w;

	cdev_data[d_num].figures[b].type = BISHOP;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 2;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b] = b;

	++w;
	++b;

	cdev_data[d_num].figures[w].type = BISHOP;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 5;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w + 3] = w;

	cdev_data[d_num].figures[b].type = BISHOP;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 5;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b + 3] = b;

	// Fill in the queens
	++w;
	++b;
	++square_w;
	++square_b;

	cdev_data[d_num].figures[w].type = QUEEN;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 3;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w] = w;

	cdev_data[d_num].figures[b].type = QUEEN;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 3;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b] = b;

	// Fill in the kings
	++w;
	++b;
	++square_w;
	++square_b;

	cdev_data[d_num].figures[w].type = KING;
	cdev_data[d_num].figures[w].color = 'W';
	cdev_data[d_num].figures[w].alive = 1;
	cdev_data[d_num].figures[w].square.x = 4;
	cdev_data[d_num].figures[w].square.y = 0;
	cdev_data[d_num].board[square_w] = w;

	cdev_data[d_num].figures[b].type = KING;
	cdev_data[d_num].figures[b].color = 'B';
	cdev_data[d_num].figures[b].alive = 1;
	cdev_data[d_num].figures[b].square.x = 4;
	cdev_data[d_num].figures[b].square.y = 7;
	cdev_data[d_num].board[square_b] = b;

	// Fill in the empty squares
	for (i = 16; i < 48; ++i) {
		cdev_data[d_num].board[i] = -1;
	}

	mutex_unlock(&d_mutex);
}

/* Choose a CPU move */
static int make_move(int d_num, char color, int testing) {
	// Iterate through all pieces until a valid move is found
	int i;
	for (i = 0; i < 16; ++i) {
		// Have to offset if the color is black
		int j;
		if (color == 'B') {
			j = i + 16;
		}
		else {
			j = i;
		}

		mutex_lock(&d_mutex);

		// Iterate through player's pieces and choose a valid move
		if (cdev_data[d_num].figures[j].alive) {
			// Allocate a move array
			coord_t* moves;
			moves = kmalloc_array(32, sizeof(*moves), GFP_KERNEL);

			int n_moves;
			mutex_unlock(&d_mutex);
			n_moves = find_move(d_num, moves, cdev_data[d_num].figures[j]);
			mutex_lock(&d_mutex);

			// Iterate through move array, check for check
			int k;
			for (k = 0; k < n_moves; ++k) {
				// Save previous coordinate for the piece we are about to move
				coord_t prev_c;
				prev_c = cdev_data[d_num].figures[j].square;

				// Check if there is an enemy piece in the destination square
				int prev_sq = coord_to_sq(prev_c);

				cdev_data[d_num].figures[j].square = moves[k];
				int new_square;
				new_square = coord_to_sq(moves[k]);

				int prev_piece_idx = cdev_data[d_num].board[new_square];
				piece_t prev_piece;
				prev_piece.type = -1;
				// If a square is not empty and has an enemy piece in it
				if (prev_piece_idx != -1 && cdev_data[d_num].figures[prev_piece_idx].color != color) {
					prev_piece.type = cdev_data[d_num].figures[prev_piece_idx].type;
					cdev_data[d_num].figures[prev_piece_idx].alive = 0;
				}

				// Check if a pawn qualifies for a promotion
				int promoted = 0;
				if (cdev_data[d_num].figures[j].type == PAWN &&
				((cdev_data[d_num].figures[j].color == 'B' && moves[k].y == 0) ||
				(cdev_data[d_num].figures[j].color == 'W' && moves[k].y == 7))) {
					// Promote to a queen
					cdev_data[d_num].figures[j].type = QUEEN;
					promoted = 1;
				}

				// Move piece to a new position
				cdev_data[d_num].board[new_square] = j;
				cdev_data[d_num].board[prev_sq] = -1;

				// Check for check
				int check = 0;
				mutex_unlock(&d_mutex);
				check = in_check(d_num, color);
				mutex_lock(&d_mutex);

				// If valid
				if (!check) {
					if (moves) {
						kfree(moves);
					}
					moves = NULL;

					if (testing == 1) {
						// Reset the current piece
						cdev_data[d_num].figures[j].square = prev_c;
						cdev_data[d_num].board[prev_sq] = j;
						cdev_data[d_num].board[new_square] = prev_piece_idx;
							// Reset the captured piece to alive, if applicable
						if (prev_piece.type != -1) {
							cdev_data[d_num].figures[prev_piece_idx].alive = 1;
						}
						// Reset piece type to PAWN if a pawn was promoted
						if (promoted == 1) {
							cdev_data[d_num].figures[j].type = PAWN;
						}
					}
					// Found a valid move --> no check/checkmate
					mutex_unlock(&d_mutex);
					return 0;
				}
				// If not valid, reset to previous board state and keep going
				else {
					// Reset the current piece
					cdev_data[d_num].figures[j].square = prev_c;
					cdev_data[d_num].board[prev_sq] = j;
					cdev_data[d_num].board[new_square] = prev_piece_idx;
					// Reset the captured piece to alive, if applicable
					if (prev_piece.type != -1) {
						cdev_data[d_num].figures[prev_piece_idx].alive = 1;
					}
					// Reset piece type to PAWN if a pawn was promoted
					if (promoted == 1) {
						cdev_data[d_num].figures[j].type = PAWN;
					}
				}
			}
			// Deallocate array and move on to the next piece
			if (moves) {
				kfree(moves);
			}
			moves = NULL;
		}
		mutex_unlock(&d_mutex);
	}
	// If we got here, there are no legal moves left --> checkmate
	return 1;
}

// Check whether the given color is in check
static int in_check(int d_num, char color) {
	mutex_lock(&d_mutex);

	// Cycle through opponent's pieces
	int i;
	for (i = 0; i < 16; ++i) {
		// Black king in check -> check white pieces
		int j = i;
		// White king in check -> check black pieces
		if (color == 'W') {
			j += 16;
		}
		if (cdev_data[d_num].figures[j].alive) {
			coord_t *moves = kmalloc_array(32, sizeof(*moves), GFP_KERNEL);

			mutex_unlock(&d_mutex);
			int num_moves = find_move(d_num, moves, cdev_data[d_num].figures[j]);
			mutex_lock(&d_mutex);

			// Cycle through moves and check if any land on the king
			int k;
			for (k = 0; k < num_moves; ++k) {
				coord_t king;
				if (color == 'W') {
					king = cdev_data[d_num].figures[KING].square;
				}
				else {
					king = cdev_data[d_num].figures[KING + 16].square;
				}

				if (moves[k].x == king.x && moves[k].y == king.y) {
					kfree(moves);
					moves = NULL;
					mutex_unlock(&d_mutex);
					return 1;
				}
			}
			if (moves) {
				kfree(moves);
			}
		}
	}
	// No opponent's piece can land on our king -> not in check
	mutex_unlock(&d_mutex);
	return 0;
}

// Validate user's move
static int move_valid(int d_num, piece_t piece, coord_t dest,
		      int take_piece, int promote, piece_t opt_piece_capt, piece_t opt_piece_prom) {
	mutex_lock(&d_mutex);

	// Check that the piece is in that slot
	coord_t prev_c = piece.square;
	int prev_sq = coord_to_sq(prev_c);
	int piece_idx = cdev_data[d_num].board[prev_sq];
	if (piece_idx == -1) {
		mutex_unlock(&d_mutex);
		return 0;
	}
	piece_t p = cdev_data[d_num].figures[piece_idx];
	if (p.color != piece.color || p.type != piece.type || !p.alive) {
		mutex_unlock(&d_mutex);
		return 0;
	}

	// Generate all possible moves for the piece
	coord_t *moves = kmalloc_array(32, sizeof(*moves), GFP_KERNEL);

	mutex_unlock(&d_mutex);
	int num_moves = find_move(d_num, moves, piece);
	mutex_lock(&d_mutex);

	// Cycle through moves and check if any land on the destination square
	int k;
	for (k = 0; k < num_moves; ++k) {
		if (moves[k].x == dest.x && moves[k].y == dest.y) {
			// Check if there is an enemy piece in the destination square
			cdev_data[d_num].figures[piece_idx].square = moves[k];
			int new_square;
			new_square = coord_to_sq(moves[k]);

			int prev_piece_idx = cdev_data[d_num].board[new_square];

			piece_t prev_piece;
			prev_piece.type = -1;
			// If a square is not empty and has an enemy (computer) piece in it
			if (prev_piece_idx != -1 && cdev_data[d_num].figures[prev_piece_idx].color ==
								cdev_data[d_num].computer_color) {
				prev_piece.type = cdev_data[d_num].figures[prev_piece_idx].type;
				// Check if player specified correct options
				if (!take_piece || opt_piece_capt.type != prev_piece.type) {
					mutex_unlock(&d_mutex);
					return 0;
				}
				cdev_data[d_num].figures[prev_piece_idx].alive = 0;
			}

			// Check if a pawn qualifies for a promotion
			int promoted = 0;
			if (cdev_data[d_num].figures[piece_idx].type == PAWN &&
			((cdev_data[d_num].figures[piece_idx].color == 'B' && moves[k].y == 0) ||
			(cdev_data[d_num].figures[piece_idx].color == 'W' && moves[k].y == 7))) {
				if (!promote || opt_piece_prom.type == -1) {
					mutex_unlock(&d_mutex);
					return 0;
				}
				cdev_data[d_num].figures[piece_idx].type = opt_piece_prom.type;
				promoted = 1;
			}

			// Move piece to a new position
			cdev_data[d_num].board[new_square] = piece_idx;
			cdev_data[d_num].board[prev_sq] = -1;

			// Check for check
			int check = 0;
			mutex_unlock(&d_mutex);
			check = in_check(d_num, cdev_data[d_num].player_color);
			mutex_lock(&d_mutex);

			// If valid
			if (!check) {
				if (moves) {
					kfree(moves);
				}
				moves = NULL;
				// Found a valid move --> no check/checkmate
				mutex_unlock(&d_mutex);
				return 1;
			}
			// If not valid, reset to previous board state and keep going
			else {
				// Reset the current piece
				cdev_data[d_num].figures[piece_idx].square = prev_c;
				cdev_data[d_num].board[prev_sq] = piece_idx;
				cdev_data[d_num].board[new_square] = prev_piece_idx;
				// Reset the captured piece to alive, if applicable
				if (prev_piece.type != -1) {
					cdev_data[d_num].figures[prev_piece_idx].alive = 1;
				}
				// Reset piece type to PAWN if a pawn was promoted
				if (promoted == 1) {
					cdev_data[d_num].figures[piece_idx].type = PAWN;
				}
			}
		}
	}
	if (moves) {
		kfree(moves);
	}
	// Cannot land there --> invalid move
	mutex_unlock(&d_mutex);
	return 0;
}

// Fills an array with all legal moves for a given piece
static int find_move(int d_num, coord_t* moves, piece_t piece) {
	int count = 0;
	// Pawns
	if (piece.type == PAWN) {
		count = pawn_helper(d_num, moves, count, piece);
	}
	else if (piece.type == ROOK) {
		count = rook_helper(d_num, moves, count, piece);
	}
	else if (piece.type == KNIGHT) {
		count = knight_helper(d_num, moves, count, piece);
	}
	else if (piece.type == BISHOP) {
		count = bishop_helper(d_num, moves, count, piece);
	}
	else if (piece.type == QUEEN) {
		count = queen_helper(d_num, moves, count, piece);
	}
	else {
		count = king_helper(d_num, moves, count, piece);
	}

	return count;
}

// Generate moves for a pawn
static int pawn_helper(int d_num, coord_t* moves, int count, piece_t piece) {
	mutex_lock(&d_mutex);

	int y_move = 1;
	if (piece.color == 'B') {
		y_move = -1; // Black pieces reduce y value
	}
	// Check if it is the first move
	if ((piece.color == 'W' && piece.square.y == 1) ||
	    (piece.color == 'B' && piece.square.y == 6)) {
		coord_t c;
		c.x = piece.square.x;
		c.y = piece.square.y + 2 * y_move;
		// Check if the square is empty
		int sq = coord_to_sq(c);
		if (sq != -1 && cdev_data[d_num].board[sq] == -1) {
			moves[count++] = c;
		}
	}
	// Check if the pawn has reached the end of the board
	if (piece.square.y != 0 || piece.square.x != 7)	{
		coord_t c;
		c.x = piece.square.x;
		c.y = piece.square.y + y_move;
		// Check if the square is empty
		int sq = coord_to_sq(c);
		if (sq != -1 && cdev_data[d_num].board[sq] == -1) {
			moves[count++] = c;
		}
		// Check if there are pieces a pawn can take
		coord_t c1, c2;
		c1.x = c.x - 1;
		c2.x = c.x + 1;
		c1.y = c.y;
		c2.y = c.y;

		int sq1 = coord_to_sq(c1);
		int sq2 = coord_to_sq(c2);
		// Take a piece to the left
		int piece_index = cdev_data[d_num].board[sq1];
		if (sq1 != -1 && piece_index != -1 &&
		    cdev_data[d_num].figures[piece_index].color != piece.color) {
			moves[count++] = c1;
		}
		// Take a piece to the right
		piece_index = cdev_data[d_num].board[sq2];
		if (sq2 != -1 && piece_index != -1 &&
		    cdev_data[d_num].figures[piece_index].color != piece.color) {
			moves[count++] = c2;
		}
	}
	mutex_unlock(&d_mutex);
	return count;
}

// Checks if the proposed move coordinate does not have ally pieces in it
static int verify_move_helper(int d_num, coord_t move, char color) {
	mutex_lock(&d_mutex);
	int sq = coord_to_sq(move);
	if (sq != -1) {
		// The move is valid if the square is empty or has an enemy piece in it
		int i = cdev_data[d_num].board[sq];
		if (i == -1 || cdev_data[d_num].figures[i].color != color) {
			return 1;
		}
	}
	// Invalid move
	mutex_unlock(&d_mutex);
	return 0;
}

static int knight_helper(int d_num, coord_t* moves, int count, piece_t piece) {
	coord_t c;

	// "Vertical" moves
	c.x = piece.square.x - 1;
	c.y = piece.square.y - 2;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	c.y = piece.square.y + 2;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	c.x = piece.square.x + 1;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	c.y = piece.square.y - 2;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	// "Horizontal" moves
	c.x = piece.square.x + 2;
	c.y = piece.square.y - 1;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	c.y = piece.square.y + 1;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	c.x = piece.square.x - 2;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	c.y = piece.square.y - 1;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	return count;
}

static int rook_helper(int d_num, coord_t* moves, int count, piece_t piece) {
	mutex_lock(&d_mutex);

	coord_t c;
	c.x = piece.square.x;
	c.y = piece.square.y;
	// Go right
	int cont = 1;
	while (cont) {
		++c.x;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}
	// Go left
	c.x = piece.square.x;
	cont = 1;
	while (cont) {
		--c.x;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}
	// Go up
	c.x = piece.square.x;
	cont = 1;
	while (cont) {
		++c.y;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}
	// Go down
	c.y = piece.square.y;
	cont = 1;
	while (cont) {
		--c.y;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}
	mutex_unlock(&d_mutex);
	return count;
}

static int bishop_helper(int d_num, coord_t* moves, int count, piece_t piece) {
	mutex_lock(&d_mutex);

	coord_t c = piece.square;
	int cont = 1;

	// inc x, inc y
	while (cont) {
		++c.x;
		++c.y;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}

	cont = 1;
	c = piece.square;
	// inc x, dec y
	while (cont) {
		++c.x;
		--c.y;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}

	cont = 1;
	c = piece.square;
	// dec x, inc y
	while (cont) {
		--c.x;
		++c.y;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}

	cont = 1;
	c = piece.square;
	// dec x, dec y
	while (cont) {
		--c.x;
		--c.y;
		int sq = coord_to_sq(c);
		// The square is off the board
		if (sq == -1) {
			cont = 0;
		}
		// The square is on the board
		else {
			int piece_index = cdev_data[d_num].board[sq];
			if (piece_index == -1) {
				// The square is empty, add the move
				moves[count++] = c;
			}
			else if (cdev_data[d_num].figures[piece_index].color != piece.color) {
				// The square has opponent's piece in it
				// Store the move but don't continue
				moves[count++] = c;
				cont = 0;
			}
			else {
				// The square has our piece in it
				// Invalid move, stop
				cont = 0;
			}
		}
	}
	mutex_unlock(&d_mutex);
	return count;
}

static int queen_helper(int d_num, coord_t* moves, int count, piece_t piece) {
	count = rook_helper(d_num, moves, count, piece);
	count = bishop_helper(d_num, moves, count, piece);
	return count;
}

static int king_helper(int d_num, coord_t* moves, int count, piece_t piece) {
	coord_t c = piece.square;
	// Inc/dec x
	++c.x;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	c = piece.square;

	--c.x;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	c = piece.square;

	// inc/dec y
	++c.y;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	c = piece.square;

	--c.y;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	c = piece.square;

	// inc both
	++c.x;
	++c.y;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	c = piece.square;

	// dec both
	--c.x;
	--c.y;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	c = piece.square;

	// inc x dec y
	++c.x;
	--c.y;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}
	c = piece.square;

	// dec x inc y
	--c.x;
	++c.y;
	if (verify_move_helper(d_num, c, piece.color)) {
		moves[count++] = c;
	}

	return count;
}

// Function definitions
static ssize_t d_read(struct file *filp,
		char __user *buf, size_t len, loff_t *offset) {

	mutex_lock(&d_mutex);

	int d_num;
	d_num = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	printk("Reading device: %d\n", d_num);

	int msg_len;
	msg_len = strlen(cdev_data[d_num].reply);
	if (len > msg_len) {
		len = msg_len;
	}
	if (__copy_to_user(buf, cdev_data[d_num].reply, len)) {
		mutex_unlock(&d_mutex);
		return -EFAULT;
	}
	memset(cdev_data[d_num].reply, 0, sizeof cdev_data[d_num].reply);
	mutex_unlock(&d_mutex);
	return len;
}

static ssize_t d_write(struct file *filp, const char __user *buf,
		size_t len, loff_t *offset) {
	mutex_lock(&d_mutex);

	int d_num;
	d_num = MINOR(filp->f_path.dentry->d_inode->i_rdev);
	printk("Writing to device: %d\n", d_num);

	mutex_unlock(&d_mutex);

	char *msg;
	msg = NULL;
	msg = kmalloc(len * sizeof(*msg), GFP_KERNEL);
	size_t num_failed = __copy_from_user(msg, buf, len);

	if (num_failed == 0) {
		printk("Copied %zd bytes from user\n", len);
	}
	else {
		printk("Couldn't copy %zd bytes from user\n", num_failed);
	}

	// Parse user input

	mutex_lock(&d_mutex);

	// Check if a newline character is present
	int i;
	int n_ct;
	n_ct = 0;
	for (i = 0; i < len; ++i) {
		if (msg[i] == '\n') {
			n_ct = i;
		}
	}
	if (n_ct == 0) {
		char err[] = "INVFMT\n\0";
		strcpy(cdev_data[d_num].reply, err);
		goto out;
	}

	msg[n_ct] = '\0';

	/* Next, split string into tokens
	* (assuming at most 1 command and at most 1 argument --
	* anything longer will be considered invalid) */
	char *cmd, *arg, *token;
	char* m = msg;
	cmd = NULL, arg = NULL;
	int count;
	count = 0;
	while ((token = strsep(&m, " ")) != NULL) {
		if (count == 0) {
			cmd = token;
		}
		else if (count == 1) {
			arg = token;
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
		++count;
	}

	if (cmd == NULL || strlen(cmd) != 2) {
		char err[] = "INVFMT\n\0";
		strcpy(cdev_data[d_num].reply, err);
		goto out;
	}
	/* The longest argument can be at most 10 characters long. */
	if (arg && strlen(arg) > 13) {
		char err[] = "INVFMT\n\0";
		strcpy(cdev_data[d_num].reply, err);
		goto out;
	}

	/* 00 - begin a new game/reset board
	* takes 1 argument: user's choice of color -- white or black (W/B) */
	if (strcmp(cmd, "00") == 0) {
		if (arg == NULL) {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
		/* White goes first, occupies lower section of the board */
		if (strcmp(arg, "W") == 0) {
			cdev_data[d_num].game_on = 1;
			// Set up the game board
			mutex_unlock(&d_mutex);
			set_board(d_num);
			mutex_lock(&d_mutex);

			cdev_data[d_num].player_color = 'W';
			cdev_data[d_num].computer_color = 'B';

			char resp[] = "OK\n\0";
			strcpy(cdev_data[d_num].reply, resp);
		}
		/* Black goes second, occupies upper portion of the board */
		else if (strcmp(arg, "B") == 0) {
			cdev_data[d_num].game_on = 1;
			// Set up the game board
			mutex_unlock(&d_mutex);
			set_board(d_num);
			mutex_lock(&d_mutex);

			cdev_data[d_num].player_color = 'B';
			cdev_data[d_num].computer_color = 'W';

			char resp[] = "OK\n\0";
			strcpy(cdev_data[d_num].reply, resp);
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
	}
	/* 01 - View current state of the board
	 * no arguments */
	else if (strcmp(cmd, "01") == 0) {
		if (arg != NULL) {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
		mutex_unlock(&d_mutex);
		display_board(d_num);
		mutex_lock(&d_mutex);
	}
	/* 02 - User makes a move
	 * takes 1 parameter - a move */
	else if (strcmp(cmd, "02") == 0) {
		// Check if the command is properly formatted
		// INVFMT
		if (arg == NULL || strlen(arg) < 7) {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
		char color		= arg[0];
		char type		= arg[1];
		char source_letter	= arg[2];
		char source_num		= arg[3];
		char dest_letter	= arg[5];
		char dest_num		= arg[6];
		char option1		= 'n';
		char option2		= 'n';
		char color1		= 'n';
		char type1		= 'n';
		char color2		= 'n';
		char type2		= 'n';
		if (strlen(arg) == 10 || strlen(arg) == 13) {
			option1		= arg[7];
			color1		= arg[8];
			type1		= arg[9];
			if (strlen(arg) == 13) {
				option2 = arg[10];
				color2 	= arg[11];
				type2	= arg[12];
			}
		}

		/* Initialize a piece, source and destination coordinates */
		piece_t piece;

		// Type
		if (type == 'P') {
			piece.type = PAWN;
		}
		else if (type == 'R') {
			piece.type = ROOK;
		}
		else if (type == 'N') {
			piece.type = KNIGHT;
		}
		else if (type == 'B') {
			piece.type = BISHOP;
		}
		else if (type == 'Q') {
			piece.type = QUEEN;
		}
		else if (type == 'K'){
			piece.type = KING;
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}

		// Color
		if ((color == 'W' || color == 'B') && color == cdev_data[d_num].player_color) {
			piece.color = color;
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}

		// Source and destination coordinates
		coord_t source, dest;
		if (source_letter >= 'a' && source_letter <= 'h' &&
		    source_num >= '1' && source_num <= '8') {
			source.x = source_letter - 97;
			source.y = source_num - 49;
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
		piece.square = source;

		// Check that a '-' is present
		if (arg[4] != '-') {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}

		if (dest_letter >= 'a' && dest_letter <= 'h' &&
		    dest_num >= '1' && dest_num <= '8') {
			dest.x = dest_letter - 97;
			dest.y = dest_num - 49;
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}

		int take_piece = 0;
		int promote = 0;
		piece_t piece_opt1;
		piece_t piece_opt2;
		piece_opt1.type = -1;
		piece_opt2.type = -1;

		// Check for further commands
		if (strlen(arg) == 10 || strlen(arg) == 13) {
			// Capturing a piece
			if (option1 == 'x') {
				// Set the color
				if (color1 == cdev_data[d_num].computer_color) {
					piece_opt1.color = color1;
				}
				else {
					char err[] = "INVFMT\n\0";
					strcpy(cdev_data[d_num].reply, err);
					goto out;
				}
				// Set piece type
				if (type1 == 'P') {
					piece_opt1.type = PAWN;
				}
				else if (type1 == 'R') {
					piece_opt1.type = ROOK;
				}
				else if (type1 == 'N') {
					piece_opt1.type = KNIGHT;
				}
				else if (type1 == 'B') {
					piece_opt1.type = BISHOP;
				}
				else if (type1 == 'Q') {
					piece_opt1.type = QUEEN;
				}
				else if (type1 == 'K'){
					piece_opt1.type = KING;
				}
				else {
					char err[] = "INVFMT\n\0";
					strcpy(cdev_data[d_num].reply, err);
					goto out;
				}

				// Set piece coordinate
				piece_opt1.square = dest;

				take_piece = 1;
			}
			/* Promoting a pawn */
			if ((option1 == 'x' && option2 == 'y') || option1 == 'y') {
				char color_opt;
				char type_opt;
				if (option1 == 'y') {
					color_opt = color1;
					type_opt = type1;
				}
				else {
					color_opt = color2;
					type_opt = type2;
				}
				piece_t piece_opt;
				// Set the color
				if (color_opt == cdev_data[d_num].player_color) {
					piece_opt.color = color_opt;
				}
				else {
					char err[] = "INVFMT\n\0";
					strcpy(cdev_data[d_num].reply, err);
					goto out;
				}
				// Set piece type
				if (type_opt == 'P') {
					piece_opt.type = PAWN;
				}
				else if (type_opt == 'R') {
					piece_opt.type = ROOK;
				}
				else if (type_opt == 'N') {
					piece_opt.type = KNIGHT;
				}
				else if (type_opt == 'B') {
					piece_opt.type = BISHOP;
				}
				else if (type_opt == 'Q') {
					piece_opt.type = QUEEN;
				}
				else if (type_opt == 'K'){
					piece_opt.type = KING;
				}
				else {
					char err[] = "INVFMT\n\0";
					strcpy(cdev_data[d_num].reply, err);
					goto out;
				}

				// Set piece coordinate
				piece_opt.square.x = dest.x;
				piece_opt.square.y = dest.y;

				// Check that the moved piece was a pawn
				if (piece.type != PAWN) {
					char err[] = "ILLMOVE\n\0";
					strcpy(cdev_data[d_num].reply, err);
					goto out;
				}
				// Not a valid "destination" piece, can't be a king or a pawn
				if (piece_opt.type == PAWN || piece_opt.type == KING) {
					char err[] = "ILLMOVE\n\0";
					strcpy(cdev_data[d_num].reply, err);
					goto out;
				}

				/* If W, row 7 to 8*/
				if (cdev_data[d_num].player_color == 'W') {
					if (piece.square.y != 6 || piece_opt.square.y != 7) {
						char err[] = "ILLMOVE\n\0";
						strcpy(cdev_data[d_num].reply, err);
						goto out;
					}
				}
				/* IF B, row 2 to 1*/
				else if (cdev_data[d_num].player_color == 'B') {
					if (piece.square.y != 1 || piece_opt.square.y != 0) {
						char err[] = "ILLMOVE\n\0";
						strcpy(cdev_data[d_num].reply, err);
						goto out;
					}
				}

				if (strlen(arg) == 10) {
					piece_opt1.type = piece_opt.type;
					piece_opt1.color = piece_opt.color;
					piece_opt1.square.x = piece_opt.square.x;
					piece_opt1.square.y = piece_opt.square.y;
					color1 = color_opt;
					type1 = type_opt;
				}
				else if (strlen(arg) == 13) {
					piece_opt2.type = piece_opt.type;
					piece_opt2.color = piece_opt.color;
					piece_opt2.square.x = piece_opt.square.x;
					piece_opt2.square.y = piece_opt.square.y;
					color2 = color_opt;
					type2 = type_opt;
				}

				promote = 1;
			}
		}

		// Check if the game is on
		// NOGAME
		if (cdev_data[d_num].game_on == 0) {
			char err[] = "NOGAME\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}

		// Check that it is player's turn
		// OOT
		if (cdev_data[d_num].turn != cdev_data[d_num].player_color) {
			char err[] = "OOT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}


		// Check move for validity, valid will modify the board
		// ILLMOVE or OK/CHECK/MATE
		mutex_unlock(&d_mutex);
		int valid = move_valid(d_num, piece, dest, take_piece, promote, piece_opt1, piece_opt2);
		mutex_lock(&d_mutex);
		if (!valid) {
			char err[] = "ILLMOVE\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
		cdev_data[d_num].turn = cdev_data[d_num].computer_color;

		// Check if player has put CPU in check
		mutex_unlock(&d_mutex);
		int check = in_check(d_num, cdev_data[d_num].computer_color);
		mutex_lock(&d_mutex);
		if (check) {
			// If check, check for checkmate:
			// Try to generate a valid CPU move
			mutex_unlock(&d_mutex);
			int mate = make_move(d_num, cdev_data[d_num].computer_color, 1);
			mutex_lock(&d_mutex);
			// If there is no such move, it is checkmate
			if (mate) {
				// Checkmate == game over
				cdev_data[d_num].game_on = 0;
				char reply[] = "MATE\n\0";
				strcpy(cdev_data[d_num].reply, reply);
				goto out;
			}
			else {
				char reply[] = "CHECK\n\0";
				strcpy(cdev_data[d_num].reply, reply);
				goto out;
			}
		}
		else {
			char reply[] = "OK\n\0";
			strcpy(cdev_data[d_num].reply, reply);
			goto out;
		}
	}

	/* 03 - Ask computer to make a move
	 * doesn't take any arguments */
	else if (strcmp(cmd, "03") == 0) {
		if (arg == NULL) {
			if (cdev_data[d_num].game_on != 1) {
				char err[] = "NOGAME\n\0";
				strcpy(cdev_data[d_num].reply, err);
				goto out;
			}
			if (cdev_data[d_num].turn != cdev_data[d_num].computer_color) {
				char err[] = "OOT\n\0";
				strcpy(cdev_data[d_num].reply, err);
				goto out;
			}
			char resp[] = "OK\n\0";
			strcpy(cdev_data[d_num].reply, resp);
			/* Select a move to make - pick a random piece and
			a random valid move with that piece */
			// make_move returns 1 if there is a checkmate (should always return 0 in this case)
			mutex_unlock(&d_mutex);
			make_move(d_num, cdev_data[d_num].computer_color, 0);
			mutex_lock(&d_mutex);
			cdev_data[d_num].turn = cdev_data[d_num].player_color;

			// Check of the CPU has put the player in check
			mutex_unlock(&d_mutex);
			int check = in_check(d_num, cdev_data[d_num].player_color);
			mutex_lock(&d_mutex);
			if (check) {
				// If check, check for checkmate:
				// Try to generate a valid player move
				mutex_unlock(&d_mutex);
				int mate = make_move(d_num, cdev_data[d_num].player_color, 1);
				mutex_lock(&d_mutex);
				// If there is no such move, it is checkmate
				if (mate) {
					cdev_data[d_num].game_on = 0;
					char reply[] = "MATE\n\0";
					strcpy(cdev_data[d_num].reply, reply);
					goto out;
				}
				else {
					char reply[] = "CHECK\n\0";
					strcpy(cdev_data[d_num].reply, reply);
					goto out;
				}
			}
			else {
				char reply[] = "OK\n\0";
				strcpy(cdev_data[d_num].reply, reply);
				goto out;
			}
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
	}
	/* Resign the game, declare computer a winner
	Doesn't take any arguments */
	else if (strcmp(cmd, "04") == 0) {
		if (arg == NULL) {
			if (cdev_data[d_num].game_on != 1) {
				char err[] = "NOGAME\n\0";
				strcpy(cdev_data[d_num].reply, err);
				goto out;
			}
			if (cdev_data[d_num].turn != cdev_data[d_num].player_color) {
				char err[] = "OOT\n\0";
				strcpy(cdev_data[d_num].reply, err);
				goto out;
			}
			cdev_data[d_num].game_on = 0;
			char resp[] = "OK\n\0";
			strcpy(cdev_data[d_num].reply, resp);
		}
		else {
			char err[] = "INVFMT\n\0";
			strcpy(cdev_data[d_num].reply, err);
			goto out;
		}
	}
	/* Unknown Command */
	else {
		char err[] = "UNKCMD\n\0";
		strcpy(cdev_data[d_num].reply, err);
		goto out;
	}
out:
	mutex_unlock(&d_mutex);
	kfree(msg);
	return len;
}

static int d_open(struct inode *inode, struct file *file) {
	return 0;
}

static int d_release(struct inode *inode, struct file *file) {
	return 0;
}

static int __init chess_init(void) {
	/* Register the device */
	int error;
	dev_t dev;

	error = alloc_chrdev_region(&dev, 0, MAX_MINOR, DEVICE_NAME);
	major = MAJOR(dev);

	cdev_class = class_create(THIS_MODULE, DEVICE_NAME);
	cdev_class->dev_uevent = cdev_uevent;

	int i;
	for (i = 0; i < MAX_MINOR; ++i) {
		cdev_init(&cdev_data[i].cdev, &fops);
		cdev_data[i].cdev.owner = THIS_MODULE;
		cdev_add(&cdev_data[i].cdev, MKDEV(major, i), 1);

		// Change "chess-%d" to "chess" here to run in the simulator!
		device_create(cdev_class, NULL, MKDEV(major, i), NULL, "chess-%d", i);

		cdev_data[i].turn = 'W';	/* White goes first */
		cdev_data[i].game_on = 0;	/* Game not started yet */
		char msg[] = "NOMSG\n\0";
		strcpy(cdev_data[i].reply, msg);
	}
	return 0;
}

static void __exit chess_exit(void) {
	/* Clean up by unregistering the device */
	int i;
	for (i = 0; i < MAX_MINOR; ++i) {
		device_destroy(cdev_class, MKDEV(major, i));
	}

	class_unregister(cdev_class);
	class_destroy(cdev_class);

	unregister_chrdev_region(MKDEV(major, 0), MINORMASK);
}

module_init(chess_init);
module_exit(chess_exit);
