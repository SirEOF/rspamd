/*
 * Copyright (c) 2009-2012, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This is implementation of metaphone algorithm that was originally written by
 * Michael G Schwern <schwern@pobox.com> as perl XS module
 */

/*
 * I suppose I could have been using a character pointer instead of
 * accesssing the array directly... 
 */

#include "config.h"
#include "metaphone.h"

/*
 * Look at the next letter in the word 
 */
#define Next_Letter (g_ascii_toupper (word[w_idx+1]))
/*
 * Look at the current letter in the word 
 */
#define Curr_Letter (g_ascii_toupper(word[w_idx]))
/*
 * Go N letters back. 
 */
#define Look_Back_Letter(n) (w_idx >= n ? g_ascii_toupper(word[w_idx-n]) : '\0')
/*
 * Previous letter.  I dunno, should this return null on failure? 
 */
#define Prev_Letter (Look_Back_Letter(1))
/*
 * Look two letters down.  It makes sure you don't walk off the string. 
 */
#define After_Next_Letter   (Next_Letter != '\0' ? g_ascii_toupper(word[w_idx+2]) \
                                                 : '\0')
#define Look_Ahead_Letter(n) (g_ascii_toupper(Lookahead(word+w_idx, n)))

#define  SH 	'X'
#define  TH		'0'

/*-- Character encoding array & accessing macros --*/
/* Stolen directly out of the book... */
char _codes[26] = {
	1,16,4,16,9,2,4,16,9,2,0,2,2,2,1,4,0,2,4,4,1,0,0,0,8,0
/*  a  b c  d e f g  h i j k l m n o p q r s t u v w x y z */
};


/*
 * Allows us to safely look ahead an arbitrary # of letters 
 */
/*
 * I probably could have just used strlen... 
 */
static char
Lookahead (char *word, int how_far)
{
    char            letter_ahead = '\0';	/* null by default */
    int             idx;
    for (idx = 0; word[idx] != '\0' && idx < how_far; idx++);
    /*
     * Edge forward in the string... 
     */

    letter_ahead = word[idx];	/* idx will be either == to how_far or at
				 * the end of the string */
    return letter_ahead;
}


/*
 * phonize one letter 
 */
#define Phonize(c)  {p[p_idx++] = c;}
/*
 * Slap a null character on the end of the phoned word 
 */
#define End_Phoned_Word {p[p_idx] = '\0';}
/*
 * How long is the phoned word? 
 */
#define Phone_Len   (p_idx)

/*
 * Note is a letter is a 'break' in the word 
 */
#define Isbreak(c)  (!g_ascii_isalpha(c))


gboolean
metaphone (char *word, int max_phonemes, char **phoned_word)
{
    int             w_idx = 0;	/* point in the phonization we're at. */
    int             p_idx = 0;	/* end of the phoned phrase */
	char           *p;

    /*-- Parameter checks --*/
    /*
     * Assume largest possible if we're given no limit 
     */
    if (max_phonemes == 0) {
		max_phonemes = strlen (word) * 2 + 1;
	}
	if (max_phonemes == 0) {
		return FALSE;
	}

    /*-- Allocate memory for our phoned_phrase --*/
	*phoned_word = g_malloc (max_phonemes * sizeof (char));
	p = *phoned_word;

    /*-- The first phoneme has to be processed specially. --*/
    /*
     * Find our first letter 
     */
    for (; ! g_ascii_isalpha (Curr_Letter); w_idx++) {
		/*
		* On the off chance we were given nothing but crap... 
		*/
		if (Curr_Letter == '\0') {
			End_Phoned_Word 
			return TRUE;	/* For testing */
		}
    }

    switch (Curr_Letter) {
		/*
		* AE becomes E 
		*/
		case 'A':
			if (Next_Letter == 'E') {
				Phonize ('E');
				w_idx += 2;
			}
			/*
			* Remember, preserve vowels at the beginning 
			*/
			else {
				Phonize ('A');
				w_idx++;
			}
			break;
		/*
		* [GKP]N becomes N 
		*/
		case 'G':
		case 'K':
		case 'P':
			if (Next_Letter == 'N') {
				Phonize ('N');
				w_idx += 2;
			}
			break;
		/*
		* WH becomes H, WR becomes R W if followed by a vowel 
		*/
		case 'W':
			if (Next_Letter == 'H' || Next_Letter == 'R') {
				Phonize (Next_Letter);
				w_idx += 2;
			} else if (isvowel (Next_Letter)) {
				Phonize ('W');
				w_idx += 2;
			}
			/*
			* else ignore 
			*/
			break;
		/*
		* X becomes S 
		*/
		case 'X':
			Phonize ('S');
			w_idx++;
			break;
		/*
		* Vowels are kept 
		*/
		/*
		* We did A already case 'A': case 'a': 
		*/
		case 'E':
		case 'I':
		case 'O':
		case 'U':
			Phonize (Curr_Letter);
			w_idx++;
			break;
    }



    /*
     * On to the metaphoning 
     */
    for (; Curr_Letter != '\0' && (max_phonemes == 0 || Phone_Len < max_phonemes); w_idx++) {
		/*
		* How many letters to skip because an eariler encoding handled
		* multiple letters 
		*/
		unsigned short int skip_letter = 0;


		/*
		* THOUGHT: It would be nice if, rather than having things like...
		* well, SCI.  For SCI you encode the S, then have to remember to
		* skip the C.  So the phonome SCI invades both S and C.  It would
		* be better, IMHO, to skip the C from the S part of the encoding.
		* Hell, I'm trying it. 
		*/

		/*
		* Ignore non-alphas 
		*/
		if (! g_ascii_isalpha (Curr_Letter))
			continue;

		/*
		* Drop duplicates, except CC 
		*/
		if (Curr_Letter == Prev_Letter && Curr_Letter != 'C')
			continue;

		switch (Curr_Letter) {
			/*
			* B -> B unless in MB 
			*/
			case 'B':
				if (Prev_Letter != 'M')
				Phonize ('B');
				break;
				/*
				* 'sh' if -CIA- or -CH, but not SCH, except SCHW. (SCHW is
				* handled in S) S if -CI-, -CE- or -CY- dropped if -SCI-,
				* SCE-, -SCY- (handed in S) else K 
				*/
			case 'C':
				if (MAKESOFT (Next_Letter)) {	/* C[IEY] */
					if (After_Next_Letter == 'A' && Next_Letter == 'I') {	/* CIA 
												*/
						Phonize (SH);
					}
					/*
					* SC[IEY] 
					*/
					else if (Prev_Letter == 'S') {
						/*
						* Dropped 
						*/
					} else {
						Phonize ('S');
					}
				} else if (Next_Letter == 'H') {
#ifndef USE_TRADITIONAL_METAPHONE
					if (After_Next_Letter == 'R' || Prev_Letter == 'S') {	/* Christ, 
												* School 
												*/
						Phonize ('K');
					} else {
						Phonize (SH);
					}
#else
					Phonize (SH);
#endif
					skip_letter++;
				} else {
					Phonize ('K');
				}
				break;
				/*
				* J if in -DGE-, -DGI- or -DGY- else T 
				*/
			case 'D':
				if (Next_Letter == 'G' && MAKESOFT (After_Next_Letter)) {
					Phonize ('J');
					skip_letter++;
				} else {
					Phonize ('T');
				}
				break;
				/*
				* F if in -GH and not B--GH, D--GH, -H--GH, -H---GH else
				* dropped if -GNED, -GN, else dropped if -DGE-, -DGI- or
				* -DGY- (handled in D) else J if in -GE-, -GI, -GY and not GG
				* else K 
				*/
			case 'G':
				if (Next_Letter == 'H') {
					if (!(NOGHTOF (Look_Back_Letter (3)) ||
						Look_Back_Letter (4) == 'H')) {
						Phonize ('F');
						skip_letter++;
					} else {
						/*
						* silent 
						*/
					}
				} else if (Next_Letter == 'N') {
					if (Isbreak (After_Next_Letter) ||
						(After_Next_Letter == 'E' &&
						Look_Ahead_Letter (3) == 'D')) {
						/*
						* dropped 
						*/
					} else {
						Phonize ('K');
					}
				} else if (MAKESOFT (Next_Letter) && Prev_Letter != 'G') {
					Phonize ('J');
				} else {
					Phonize ('K');
				}
				break;
				/*
				* H if before a vowel and not after C,G,P,S,T 
				*/
			case 'H':
				if (isvowel (Next_Letter) && !AFFECTH (Prev_Letter)) {
					Phonize ('H');
				}
				break;
				/*
				* dropped if after C else K 
				*/
			case 'K':
				if (Prev_Letter != 'C') {
					Phonize ('K');
				}
				break;
				/*
				* F if before H else P 
				*/
			case 'P':
				if (Next_Letter == 'H') {
					Phonize ('F');
				} else {
					Phonize ('P');
				}
				break;
				/*
				* K 
				*/
			case 'Q':
				Phonize ('K');
				break;
				/*
				* 'sh' in -SH-, -SIO- or -SIA- or -SCHW- else S 
				*/
			case 'S':
				if (Next_Letter == 'I' &&
					(After_Next_Letter == 'O' || After_Next_Letter == 'A')) {
					Phonize (SH);
				} else if (Next_Letter == 'H') {
					Phonize (SH);
					skip_letter++;
				}
#ifndef USE_TRADITIONAL_METAPHONE
				else if (Next_Letter == 'C' &&
					Look_Ahead_Letter (2) == 'H' &&
					Look_Ahead_Letter (3) == 'W') {
					Phonize (SH);
					skip_letter += 2;
				}
#endif
				else {
					Phonize ('S');
				}
				break;
				/*
				* 'sh' in -TIA- or -TIO- else 'th' before H else T 
				*/
			case 'T':
				if (Next_Letter == 'I' &&
				(After_Next_Letter == 'O' || After_Next_Letter == 'A')) {
					Phonize (SH);
				} else if (Next_Letter == 'H') {
					Phonize (TH);
					skip_letter++;
				} else {
					Phonize ('T');
				}
				break;
				/*
				* F 
				*/
			case 'V':
				Phonize ('F');
				break;
				/*
				* W before a vowel, else dropped 
				*/
			case 'W':
				if (isvowel (Next_Letter)) {
					Phonize ('W');
				}
				break;
				/*
				* KS 
				*/
			case 'X':
				Phonize ('K');
				Phonize ('S');
				break;
				/*
				* Y if followed by a vowel 
				*/
			case 'Y':
				if (isvowel (Next_Letter)) {
					Phonize ('Y');
				}
				break;
				/*
				* S 
				*/
			case 'Z':
				Phonize ('S');
				break;
				/*
				* No transformation 
				*/
			case 'F':
			case 'J':
			case 'L':
			case 'M':
			case 'N':
			case 'R':
				Phonize (Curr_Letter);
				break;
		}			/* END SWITCH */

		w_idx += skip_letter;
    }				/* END FOR */

    End_Phoned_Word;

    return TRUE;
}
