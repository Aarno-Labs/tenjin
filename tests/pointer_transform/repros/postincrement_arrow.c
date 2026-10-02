// Reduced from antcc's src/u_bits.h:bscopy (BitSet is from src/antcc.h).
typedef struct { unsigned long u; } BitSet;

void bscopy(BitSet dst[], const BitSet src[], unsigned siz)
{
    while (siz--) dst++->u = src++->u;
}
