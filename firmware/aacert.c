/* The unit's Android Auto certificate and key from the stock AA module in flash (aacert.h, notes/android_auto.md).
 *
 * JFFS2 (Linux fs/jffs2, little-endian): every node starts on a 4-byte boundary with magic 0x1985, a node type, its
 * total length and a CRC of those 8 bytes (computed with the "accurate" bit 0x2000 set: an obsoleted node has it
 * cleared on flash). Directory entries (0xE001) map a name to an inode number, the highest version wins; inode nodes
 * (0xE002) carry a span of file data at a file offset, later versions overwrite earlier ones. Every used erase block
 * starts with a node, so the walk probes each 4 KB boundary, follows the chain of node lengths from there, and resyncs
 * 4 bytes at a time over anything that is neither a node nor erased flash. JFFS2's CRC is the Linux crc32(0, ...):
 * the reflected IEEE polynomial without the pre and post inversion. */
#include "aacert.h"
#include "lzma/LzmaDec.h"

typedef unsigned int u32;
typedef unsigned char u8;

#define J_MAGIC    0x1985u
#define J_ACCURATE 0x2000u
#define J_DIRENT   0xE001u
#define J_INODE    0xE002u
#define J_COMPR_NONE 0x00u
#define J_COMPR_ZERO 0x01u
#define PROBE      0x1000u
#define MAX_NODES  4096u

static u32 jcrc(const u8 *p, u32 n)
{
    u32 c = 0;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return c;
}

static u32 le16(const u8 *p) { return p[0] | (u32)p[1] << 8; }
static u32 le32(const u8 *p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

static void cpy(u8 *d, const u8 *s, u32 n)
{
    while (n--)
        *d++ = *s++;
}

/* total length of a node with a good header at pos, else 0 */
static u32 node_at(const u8 *f, u32 n, u32 pos)
{
    if (pos > n || n - pos < 12 || le16(f + pos) != J_MAGIC)
        return 0;
    u8 h[8];
    cpy(h, f + pos, 8);
    h[3] |= J_ACCURATE >> 8;
    u32 len = le32(f + pos + 4);
    if (jcrc(h, 8) != le32(f + pos + 8) || len < 12 || len > n - pos)
        return 0;
    return len;
}

typedef void (*visit_fn)(struct aacert *a, const u8 *node, u32 pos, u32 len, u32 type);

static void walk(struct aacert *a, const u8 *f, u32 n, visit_fn v)
{
    u32 pos = 0;
    int chain = 0;
    while (pos < n && n - pos >= 12) {
        u32 len = node_at(f, n, pos);
        if (len) {
            u32 type = le16(f + pos + 2);
            if (type & J_ACCURATE)
                v(a, f + pos, pos, len, type);
            chain = 1;
            pos += (len + 3) & ~3u;
        } else if (chain && le32(f + pos) != 0xffffffffu) {
            pos += 4;   /* resync inside a used block */
        } else {
            chain = 0;
            pos = (pos | (PROBE - 1)) + 1;
        }
    }
}

/* pass 1: the newest directory entry named AACERT_NAME */
static u32 best_dirent_ver;
static void find_dirent(struct aacert *a, const u8 *d, u32 pos, u32 len, u32 type)
{
    static const char want[] = AACERT_NAME;
    u32 nsize = d[28];
    if (type != J_DIRENT || len < 40 || nsize != sizeof want - 1 || len - 40 < nsize)
        return;
    if (jcrc(d, 32) != le32(d + 32) || jcrc(d + 40, nsize) != le32(d + 36))
        return;
    for (u32 i = 0; i < nsize; i++)
        if (d[40 + i] != (u8)want[i])
            return;
    u32 ver = le32(d + 16);
    if (a->dirent == ~0u || ver >= best_dirent_ver)
        a->dirent = pos, a->ino = le32(d + 20), best_dirent_ver = ver;
}

/* pass 2: the file's good data nodes (offsets into a list in the work area), and its size from the newest */
static u32 *node_list, node_cap, best_inode_ver, bad_nodes;
static void find_nodes(struct aacert *a, const u8 *d, u32 pos, u32 len, u32 type)
{
    if (type != J_INODE || len < 68 || le32(d + 12) != a->ino)
        return;
    u32 csize = le32(d + 48), dsize = le32(d + 52), compr = d[56];
    if (jcrc(d, 60) != le32(d + 64) || len - 68 < csize) {
        bad_nodes++;
        return;
    }
    if ((compr == J_COMPR_NONE && csize != dsize) || (compr != J_COMPR_NONE && compr != J_COMPR_ZERO) ||
        (compr == J_COMPR_NONE && jcrc(d + 68, csize) != le32(d + 60))) {
        bad_nodes++;
        return;
    }
    u32 ver = le32(d + 16);
    if (!a->nodes || ver >= best_inode_ver)
        a->packed = le32(d + 28), best_inode_ver = ver;
    if (a->nodes < node_cap)
        node_list[a->nodes] = pos;
    a->nodes++;
}

struct bump {
    ISzAlloc a;
    u8 *p, *end;
};
static void *bump_alloc(ISzAllocPtr p, size_t n)
{
    struct bump *b = (struct bump *)p;
    n = (n + 7) & ~(size_t)7;
    if ((size_t)(b->end - b->p) < n)
        return 0;
    void *r = b->p;
    b->p += n;
    return r;
}
static void bump_free(ISzAllocPtr p, void *addr) { (void)p, (void)addr; }

int aacert_open(struct aacert *a, const unsigned char *flash, unsigned flash_len, void *work, unsigned work_len)
{
    u8 *w0 = (u8 *)(((unsigned long)work + 7) & ~7ul), *w = w0, *wend = (u8 *)work + work_len;
    for (u32 i = 0; i < sizeof *a; i++)
        ((u8 *)a)[i] = 0;
    a->dirent = a->first_node = ~0u;
    if (work_len < 1024)
        return a->result = AACERT_EROOM;
    best_dirent_ver = 0;
    walk(a, flash, flash_len, find_dirent);
    if (a->dirent == ~0u || !a->ino)   /* ino 0: the newest entry is an unlink */
        return a->result = AACERT_ENOMOD;

    /* the decoder state first, then the node list where the packed file will go */
    if ((u32)(wend - w) < sizeof(CLzmaDec) + 64)
        return a->result = AACERT_EROOM;
    a->dec = w;
    w += (sizeof(CLzmaDec) + 7) & ~7u;
    node_list = (u32 *)(void *)w;
    node_cap = (u32)(wend - w) / 4 < MAX_NODES ? (u32)(wend - w) / 4 : MAX_NODES;
    best_inode_ver = bad_nodes = 0;
    walk(a, flash, flash_len, find_nodes);
    if (!a->nodes || a->nodes > node_cap)
        return a->result = !a->nodes ? AACERT_ENODATA : AACERT_EROOM;
    /* newest last: sort the list by node version (a few dozen nodes) */
    for (u32 i = 1; i < a->nodes; i++) {
        u32 x = node_list[i], xv = le32(flash + x + 16), j = i;
        for (; j && le32(flash + node_list[j - 1] + 16) > xv; j--)
            node_list[j] = node_list[j - 1];
        node_list[j] = x;
    }
    /* the packed file goes after the node list (copied out of the way first) */
    u32 nn = a->nodes, list_bytes = nn * 4;
    if (a->packed < 13 || (u32)(wend - w) < list_bytes + a->packed + 16)
        return a->result = a->packed < 13 ? AACERT_EHDR : AACERT_EROOM;
    u32 *list = (u32 *)(void *)(((unsigned long)wend - list_bytes) & ~3ul);
    if ((u8 *)list < w + a->packed)
        return a->result = AACERT_EROOM;
    for (u32 i = nn; i--;)
        list[i] = node_list[i];
    a->pk = w;
    for (u32 i = 0; i < a->packed; i++)
        a->pk[i] = 0;
    for (u32 i = 0; i < nn; i++) {
        const u8 *d = flash + list[i];
        u32 off = le32(d + 44), dsize = le32(d + 52);
        if (off >= a->packed)
            continue;
        u32 k = dsize < a->packed - off ? dsize : a->packed - off;
        if (d[56] == J_COMPR_NONE)
            cpy(a->pk + off, d + 68, k);
        else
            for (u32 j = 0; j < k; j++)
                a->pk[off + j] = 0;
        if (off == 0)
            a->first_node = list[i];
    }
    /* holes: sort by file offset and sweep */
    for (u32 i = 1; i < nn; i++) {
        u32 x = list[i], xo = le32(flash + x + 44), j = i;
        for (; j && le32(flash + list[j - 1] + 44) > xo; j--)
            list[j] = list[j - 1];
        list[j] = x;
    }
    u32 covered = 0;
    for (u32 i = 0; i < nn && covered < a->packed; i++) {
        u32 off = le32(flash + list[i] + 44), e = off + le32(flash + list[i] + 52);
        if (off > covered)
            break;
        if (e > covered)
            covered = e;
    }
    if (covered < a->packed)
        return a->result = AACERT_ENODATA;
    w += (a->packed + 7) & ~7u;

    /* LZMA-alone: props, dictionary size u32, unpacked size u64. The dictionary size's low byte is 0 for any real
     * size, so the XOR key is byte 1 as stored. */
    a->xor_key = a->pk[1];
    if (a->xor_key)
        for (u32 i = 0; i < a->packed; i++)
            a->pk[i] ^= (u8)a->xor_key;
    u32 dict = le32(a->pk + 1), lo = le32(a->pk + 5), hi = le32(a->pk + 9);
    if (a->pk[0] >= 9 * 5 * 5 || dict < 4096 || hi || !lo)
        return a->result = AACERT_EHDR;
    a->unpacked = lo;

    struct bump b = { { bump_alloc, bump_free }, w, wend };
    CLzmaDec *dec = a->dec;
    LzmaDec_Construct(dec);
    SRes ar = LzmaDec_AllocateProbs(dec, a->pk, LZMA_PROPS_SIZE, &b.a);
    if (ar != SZ_OK)
        return a->result = ar == SZ_ERROR_MEM ? AACERT_EROOM : AACERT_EHDR;
    w = b.p;
    if ((u32)(wend - w) < a->unpacked)
        return a->result = AACERT_EROOM;
    a->out = w;
    a->work_used = (u32)(w - w0) + a->unpacked;
    dec->dic = a->out;
    dec->dicBufSize = a->unpacked;
    LzmaDec_Init(dec);
    a->in_pos = 13;
    return 0;
}

/* ---- PEM blocks in the unpacked module ---- */
static int b64(u8 c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=';
}

static int eq(const u8 *p, const char *s, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (p[i] != (u8)s[i])
            return 0;
    return 1;
}

/* A block at i: "-----BEGIN L-----", a newline, at least 64 base64 characters over lines, "-----END L-----".
 * 1 = [i, *end) is one (label at *lab, *lab_n long), 0 = not a block, -1 = it runs past the data unpacked so far. */
static int pem_at(const u8 *b, u32 n, u32 i, u32 *end, u32 *lab, u32 *lab_n)
{
    u32 p = i + 11, l;
    for (l = 0; p + l < n && l <= 40 && ((b[p + l] >= 'A' && b[p + l] <= 'Z') || b[p + l] == ' '); l++)
        ;
    if (p + l + 5 > n)
        return -1;
    if (!l || l > 40 || !eq(b + p + l, "-----", 5))
        return 0;
    u32 q = p + l + 5, chars = 0;
    if (q >= n)
        return -1;
    if (b[q] != '\n' && b[q] != '\r')
        return 0;
    for (; q < n && q - i < AACERT_PEM_MAX - 2; q++) {
        if (b64(b[q]))
            chars++;
        else if (b[q] != '\n' && b[q] != '\r')
            break;
    }
    if (q - i >= AACERT_PEM_MAX - 2)
        return 0;
    if (q + 9 + l + 5 > n)
        return -1;
    if (chars < 64 || b[q - 1] != '\n' || !eq(b + q, "-----END ", 9) || !eq(b + q + 9, (const char *)b + p, l) ||
        !eq(b + q + 9 + l, "-----", 5))
        return 0;
    *end = q + 9 + l + 5, *lab = p, *lab_n = l;
    return *end - i <= AACERT_PEM_MAX - 2 ? 1 : 0;
}

static u32 put_pem(char *dst, const u8 *src, u32 n)   /* the block, a newline, a NUL; returns n + 1 */
{
    cpy((u8 *)dst, src, n);
    dst[n] = '\n', dst[n + 1] = 0;
    return n + 1;
}

static int try_pair(struct aacert *a, u32 c, u32 k, aacert_check_fn check, void *ctx)
{
    u32 cl = put_pem(a->cert_out, a->out + a->cert_off[c], a->cert_n[c]);
    u32 kl = put_pem(a->key_out, a->out + a->key_off[k], a->key_n[k]);
    if (check(ctx, a->cert_out, cl + 1, a->key_out, kl + 1))
        return 0;
    a->cert_at = a->cert_off[c], a->key_at = a->key_off[k], a->cert_len = cl, a->key_len = kl;
    return 1;
}

static int scan(struct aacert *a, int final, aacert_check_fn check, void *ctx)
{
    const u8 *b = a->out;
    u32 n = a->decoded;
    while (a->scan < n) {
        u32 i = a->scan;
        while (i < n && b[i] != '-')
            i++;
        if (n - i < 11) {
            a->scan = final ? n : i;
            return 0;
        }
        u32 end, lab, ln;
        int r = eq(b + i, "-----BEGIN ", 11) ? pem_at(b, n, i, &end, &lab, &ln) : 0;
        if (r < 0 && !final) {
            a->scan = i;
            return 0;
        }
        if (r <= 0) {
            a->scan = i + 1;
            continue;
        }
        a->scan = end;
        int cert = ln == 11 && eq(b + lab, "CERTIFICATE", 11);
        int key = (ln == 11 && eq(b + lab, "PRIVATE KEY", 11)) || (ln == 15 && eq(b + lab, "RSA PRIVATE KEY", 15)) ||
                  (ln == 14 && eq(b + lab, "EC PRIVATE KEY", 14));
        if (cert && a->ncert < AACERT_MAX_PEMS) {
            a->cert_off[a->ncert] = i, a->cert_n[a->ncert] = end - i;
            for (u32 k = 0; k < a->nkey; k++)
                if (try_pair(a, a->ncert, k, check, ctx))
                    return 1;
            a->ncert++;
        } else if (key && a->nkey < AACERT_MAX_PEMS) {
            a->key_off[a->nkey] = i, a->key_n[a->nkey] = end - i;
            for (u32 c = 0; c < a->ncert; c++)
                if (try_pair(a, c, a->nkey, check, ctx))
                    return 1;
            a->nkey++;
        }
    }
    return 0;
}

int aacert_step(struct aacert *a, unsigned budget, char *cert_out, char *key_out, aacert_check_fn check, void *ctx)
{
    if (a->result)
        return a->result > 0 ? 0 : a->result;
    CLzmaDec *dec = a->dec;
    a->cert_out = cert_out, a->key_out = key_out;
    if (!budget)
        budget = 1;
    SizeT limit = dec->dicPos + (budget < a->unpacked - dec->dicPos ? budget : a->unpacked - dec->dicPos);
    SizeT in = a->packed - a->in_pos, was = dec->dicPos;
    ELzmaStatus st;
    SRes r = LzmaDec_DecodeToDic(dec, limit, a->pk + a->in_pos, &in, limit == a->unpacked ? LZMA_FINISH_END : LZMA_FINISH_ANY, &st);
    a->in_pos += (u32)in;
    if (r != SZ_OK || (dec->dicPos < limit && (st == LZMA_STATUS_NEEDS_MORE_INPUT || st == LZMA_STATUS_FINISHED_WITH_MARK)) ||
        (dec->dicPos == was && !in))
        return a->result = AACERT_EDATA;
    a->decoded = (u32)dec->dicPos;
    int final = a->decoded == a->unpacked;
    if (scan(a, final, check, ctx))
        return a->result = 1, 0;
    if (!final)
        return 1;
    return a->result = !a->ncert || !a->nkey ? AACERT_ENOPEM : AACERT_ENOPAIR;
}

const char *aacert_strerror(int e)
{
    switch (e) {
    case 0: return "ok";
    case AACERT_ENOMOD: return "no " AACERT_NAME " in a flash filesystem";
    case AACERT_ENODATA: return "the module's data nodes are missing or damaged";
    case AACERT_EHDR: return "the module is not an LZMA-alone stream";
    case AACERT_EROOM: return "the module does not fit the work area";
    case AACERT_EDATA: return "the module's LZMA data is damaged";
    case AACERT_ENOPEM: return "no private key and certificate in the module";
    case AACERT_ENOPAIR: return "no certificate in the module pairs with its key";
    default: return "?";
    }
}
