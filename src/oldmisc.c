/*
 * opera-boot (GPL-2.0-only): clean-room minimal 3DO bootstrapper.
 *
 * oldmisc.c: the C&B-era (RSA_OLD_MISCCODE) misc-code contract,
 * implemented clean-room instead of harvested.  Discs mastered before
 * the MiscCode split ship their executable list/mem/runtime-division
 * utilities inside the compressed OS image; the booted kernel reaches
 * them through a 14-slot vtable handed over in r8.  Rather than running
 * the disc's wedge-prone unpacker from dipir context to recover those
 * bodies, we provide our own equivalents with identical semantics and
 * publish their addresses in the vtable.  The buffer itself only ever
 * supplies the two-word prelude plus the table; the bodies live in this
 * blob, which persists in low DRAM across boot (the kernel calls back
 * into resident dipir code there, so the region is preserved).
 *
 * Slot order is the disc-side ABI (see ob_build_old_misc); argument
 * and return conventions follow the documented Portfolio interfaces
 * (AddHead, AddTail, RemHead, RemTail, RemNode, InsertNodeFromHead,
 * InsertNodeFromTail and InitList in the SDK list chapter) and the ARM
 * runtime divide helpers (quotient in r0, remainder in r1, first
 * argument is the divisor).
 */
#include "opera-boot.h"

typedef void (*ObMiscFn)(void);

void *ob_memcpy(void *dst, const void *src, u32 len);
void *ob_memset(void *dst, u32 val, u32 len);
void  ob_list_addhead(ObList *l, ObNode *n);
void  ob_list_addtail(ObList *l, ObNode *n);
ObNode *ob_list_remhead(ObList *l);
ObNode *ob_list_remtail(ObList *l);
void  ob_list_remnode(ObNode *n);
void  ob_list_insert_from_head(ObList *l, ObNode *n);
void  ob_list_insert_from_tail(ObList *l, ObNode *n);
void  ob_list_init(ObList *l, const char *name);
u32   ob_rt_sdiv(u32 d1, u32 d2);		/* in rtdiv.s */
u32   ob_rt_udiv(u32 d1, u32 d2);		/* in rtdiv.s */
u32   ob_rt_sdiv10(u32 n);			/* in rtdiv.s */
u32   ob_rt_udiv10(u32 n);			/* in rtdiv.s */

/* Byte copy with an aligned word fast path.  Unaligned word loads fault
 * on the ARM60, so any misalignment (including a length tail) drops to
 * the byte loop. */
void *ob_memcpy(void *dst, const void *src, u32 len)
{
	u8 *d = (u8 *)dst;
	const u8 *s = (const u8 *)src;

	if ((((u32)d & 3) == 0) && (((u32)s & 3) == 0) && ((len & 3) == 0)) {
		u32 *dw = (u32 *)d;
		const u32 *sw = (const u32 *)s;

		len >>= 2;
		while (len-- != 0)
			*dw++ = *sw++;

		return dst;
	}

	while (len-- != 0)
		*d++ = *s++;

	return dst;
}

/* Byte fill with the same aligned word fast path; the fill word is built
 * from the masked byte so the math is endian-neutral. */
void *ob_memset(void *dst, u32 val, u32 len)
{
	u8 *d = (u8 *)dst;
	u8 b = (u8)(val & 0xFF);
	u32 w = (((u32)b << 24) | ((u32)b << 16) | ((u32)b << 8) | (u32)b);

	if ((((u32)d & 3) == 0) && ((len & 3) == 0)) {
		u32 *dw = (u32 *)d;

		len >>= 2;
		while (len-- != 0)
			*dw++ = w;

		return dst;
	}

	while (len-- != 0)
		*d++ = b;

	return dst;
}

/* Anchor-link primitives.  The anchor words double as link cells: the
 * empty-list markers are the addresses of each other's storage, so a
 * uniform "*link = n" through either marker lands in the right anchor
 * word and no empty/non-empty case split is needed. */

void ob_list_addhead(ObList *l, ObNode *n)
{
	ObNode *next = l->a_first;

	n->n_Prev = (ObNode *)&l->a_first;
	n->n_Next = next;
	next->n_Prev = n;
	l->a_first = n;
}

void ob_list_addtail(ObList *l, ObNode *n)
{
	ObNode *prev = l->a_last;

	n->n_Next = (ObNode *)&l->a_tailmark;
	n->n_Prev = prev;
	prev->n_Next = n;
	l->a_last = n;
}

ObNode *ob_list_remhead(ObList *l)
{
	ObNode *n = l->a_first;
	ObNode *next;

	if (n == (ObNode *)&l->a_tailmark)
		return NULL;

	next = n->n_Next;
	l->a_first = next;
	next->n_Prev = (ObNode *)&l->a_first;

	return n;
}

ObNode *ob_list_remtail(ObList *l)
{
	ObNode *n = l->a_last;
	ObNode *prev;

	if (n == (ObNode *)&l->a_first)
		return NULL;

	prev = n->n_Prev;
	l->a_last = prev;
	prev->n_Next = (ObNode *)&l->a_tailmark;

	return n;
}

void ob_list_remnode(ObNode *n)
{
	n->n_Prev->n_Next = n->n_Next;
	n->n_Next->n_Prev = n->n_Prev;
}

/* Priority-sorted insertion.  From the head: walk while the resident
 * priority is strictly higher, splice before the first node that is not
 * (new node lands after equal priorities).  From the tail: mirror image,
 * splicing after the first node that is not strictly lower.  The cursor
 * may rest on an anchor marker; reading its link cell through the node
 * layout yields the correct opposite end, keeping one splice shape. */
void ob_list_insert_from_head(ObList *l, ObNode *n)
{
	ObNode *cur = l->a_first;
	ObNode *prev;

	while ((cur != (ObNode *)&l->a_tailmark) &&
	       (cur->n_Priority > n->n_Priority))
		cur = cur->n_Next;

	prev = cur->n_Prev;
	n->n_Next = cur;
	n->n_Prev = prev;
	prev->n_Next = n;
	cur->n_Prev = n;
}

void ob_list_insert_from_tail(ObList *l, ObNode *n)
{
	ObNode *cur = l->a_last;
	ObNode *next;

	while ((cur != (ObNode *)&l->a_first) &&
	       (cur->n_Priority < n->n_Priority))
		cur = cur->n_Prev;

	next = cur->n_Next;
	n->n_Prev = cur;
	n->n_Next = next;
	next->n_Prev = n;
	cur->n_Next = n;
}

/* Reset a list to empty with an identity node, exactly the state the
 * SDK INITLIST static initializer produces. */
void ob_list_init(ObList *l, const char *name)
{
	l->l_node.n_Next = NULL;
	l->l_node.n_Prev = NULL;
	l->l_node.n_SubsysType = OB_NST_KERNEL;
	l->l_node.n_Type = OB_NODETYPE_LIST;
	l->l_node.n_Priority = 0;
	l->l_node.n_Flags = OB_NODE_NAMEVALID;
	l->l_node.n_Size = (s32)sizeof(ObList);
	l->l_node.n_Name = name;
	l->a_first = (ObNode *)&l->a_tailmark;
	l->a_tailmark = NULL;
	l->a_last = (ObNode *)&l->a_first;
}

/* Build the misc buffer the C&B-era kernels expect: the two-word
 * prelude (r0 = table address, enter/return through r8) followed by the
 * 14 absolute entry pointers, in the discs' fixed slot order. */
void ob_build_old_misc(u32 miscaddr)
{
	static const ObMiscFn fn[OLDMISC_SLOTS] = {
		(ObMiscFn)ob_memcpy,			/*  0: memcpy                */
		(ObMiscFn)ob_memset,			/*  1: memset                */
		(ObMiscFn)ob_list_remnode,		/*  2: RemNode               */
		(ObMiscFn)ob_list_insert_from_tail,	/*  3: InsertNodeFromTail    */
		(ObMiscFn)ob_list_addtail,		/*  4: AddTail               */
		(ObMiscFn)ob_list_remtail,		/*  5: RemTail               */
		(ObMiscFn)ob_list_addhead,		/*  6: AddHead               */
		(ObMiscFn)ob_list_remhead,		/*  7: RemHead               */
		(ObMiscFn)ob_list_insert_from_head,	/*  8: InsertNodeFromHead    */
		(ObMiscFn)ob_list_init,			/*  9: InitList              */
		(ObMiscFn)ob_rt_sdiv,			/* 10: __rt_sdiv             */
		(ObMiscFn)ob_rt_udiv,			/* 11: __rt_udiv             */
		(ObMiscFn)ob_rt_sdiv10,			/* 12: __rt_sdiv10           */
		(ObMiscFn)ob_rt_udiv10			/* 13: __rt_udiv10           */
	};
	u32 *w = (u32 *)miscaddr;
	u32 i;

	*w++ = 0xE1A0000F;		/* MOV r0, pc : r0 = &fn[0]       */
	*w++ = 0xE1A0F008;		/* MOV pc, r8 : enter/return shim */

	for (i = 0; i < OLDMISC_SLOTS; i++)
		*w++ = (u32)fn[i];
}
