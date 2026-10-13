/* switch statements. Dense cases compile to jump tables. Sparse cases compile to a
 * compare tree. The offset range (cases 100 to 110) makes gcc subtract a bias
 * before the bounds check.
 */
#include "tcommon.h"

static int dense(int x)
{
    switch (x) {
    case 0: return 11;
    case 1: return 23;
    case 2: return 37;
    case 3: return 41;
    case 4:            /* fallthrough into 5 */
    case 5: return 59;
    case 6: return 61;
    case 7: return 73;
    case 8: return 89;
    case 9: return 97;
    case 10: return 101;
    case 11: return 103;
    case 12: return 107;
    case 13: return 109;
    case 14: return 113;
    case 15: return 127;
    default: return -1;
    }
}

static int sparse(int x)
{
    switch (x) {
    case 1: return 100;
    case 50: return 200;
    case 100: return 300;
    case 1000: return 400;
    case 65535: return 500;
    case -7: return 600;
    default: return 0;
    }
}

static int offset(int x)
{
    switch (x) {
    case 100: return 1;
    case 101: return 4;
    case 102: return 9;
    case 103: return 16;
    case 104: return 25;
    case 105: return 36;
    case 106: return 49;
    case 107: return 64;
    case 108: return 81;
    case 109: return 100;
    case 110: return 121;
    default: return -5;
    }
}

static int chars(char c)
{
    switch (c) {
    case 'a': case 'e': case 'i': case 'o': case 'u':
        return 1;
    case ' ':
        return 2;
    case '0': case '1': case '2': case '3': case '4':
    case '5': case '6': case '7': case '8': case '9':
        return 3;
    default:
        return 0;
    }
}

int vt_main(void)
{
    int total = 0;
    for (int i = -2; i < 18; i++) {
        int d = dense(i);
        total += d;
        kv("dense", d);
    }
    int sp[] = {1, 50, 100, 1000, 65535, -7, 2, 999};
    for (int i = 0; i < 8; i++) {
        int s = sparse(sp[i]);
        total += s;
        kv("sparse", s);
    }
    for (int i = 98; i < 113; i++) {
        int o = offset(i);
        total += o;
        kv("offset", o);
    }
    const char *text = "a text 2024 with e and o";
    int cls = 0;
    for (int i = 0; text[i]; i++) cls = cls * 3 + chars(text[i]);
    kv("chars", cls);
    kv("total", total);
    return total & 0x7F;
}
