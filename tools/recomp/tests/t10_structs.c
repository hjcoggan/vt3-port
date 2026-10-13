/* Struct access through pointers: nested structs, arrays of structs, a linked list
 * on the Win32 heap, bit-fields, unions used for type punning, and struct copies.
 */
#include "tcommon.h"

typedef struct {
    short x, y;
} point_t;

typedef struct {
    point_t pos;
    point_t vel;
    unsigned char kind : 3;
    unsigned char alive : 1;
    unsigned char flags : 4;
    unsigned int id;
    char name[12];
} entity_t;

typedef struct node {
    int value;
    struct node *next;
} node_t;

typedef union {
    float f;
    unsigned int u;
} pun_t;

static entity_t world[6];

static void step(entity_t *e)
{
    e->pos.x += e->vel.x;
    e->pos.y += e->vel.y;
    if (e->pos.x < 0 || e->pos.x > 100) e->vel.x = (short)-e->vel.x;
    if (e->pos.y < 0 || e->pos.y > 100) e->vel.y = (short)-e->vel.y;
}

static node_t *push(node_t *head, int v)
{
    node_t *n = HeapAlloc(GetProcessHeap(), 8u, sizeof *n);
    n->value = v;
    n->next = head;
    return n;
}

int vt_main(void)
{
    for (int i = 0; i < 6; i++) {
        world[i].pos.x = (short)(i * 15);
        world[i].pos.y = (short)(100 - i * 13);
        world[i].vel.x = (short)(i % 2 ? 4 : -3);
        world[i].vel.y = (short)(i + 2);
        world[i].kind = (unsigned char)(i & 7);
        world[i].alive = (unsigned char)(i != 3);
        world[i].flags = (unsigned char)(i * 3);
        world[i].id = 1000u + (unsigned int)i;
        world[i].name[0] = 'e';
        world[i].name[1] = (char)('0' + i);
        world[i].name[2] = 0;
    }
    for (int t = 0; t < 40; t++) {
        for (int i = 0; i < 6; i++) {
            if (world[i].alive) step(&world[i]);
        }
    }
    for (int i = 0; i < 6; i++) {
        entity_t *e = &world[i];
        kv("x", e->pos.x);
        kv("y", e->pos.y);
        kv("kind", e->kind);
        kv("flags", e->flags);
        kvx("id", e->id);
        out_str(e->name);
        out_nl();
    }

    /* struct copy by value (a block copy the compiler may route through memcpy) */
    entity_t copy = world[4];
    copy.id = 77u;
    kvx("copy_id", copy.id);
    kvx("orig_id", world[4].id);

    node_t *head = 0;
    for (int i = 1; i <= 10; i++) head = push(head, i * i);
    int sum = 0, n = 0;
    for (node_t *p = head; p; p = p->next) {
        sum += p->value;
        n++;
    }
    kv("list_sum", sum);
    kv("list_len", n);

    pun_t p;
    p.f = 1.0f;
    kvx("pun_bits", p.u);
    p.u ^= 0x80000000u;
    kv("pun_neg", (int)(p.f * 10.0f));
    return sum & 0x7F;
}
