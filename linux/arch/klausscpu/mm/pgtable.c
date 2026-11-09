// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/pgalloc.h>

/* The pgd is 8 entries; the user half starts empty, kmap comes from swapper. */
pgd_t *pgd_alloc(struct mm_struct *mm)
{
	pgd_t *pgd = kzalloc(PTRS_PER_PGD * sizeof(pgd_t), GFP_KERNEL);

	if (pgd)
		memcpy(pgd + USER_PTRS_PER_PGD, swapper_pg_dir + USER_PTRS_PER_PGD,
		       (PTRS_PER_PGD - USER_PTRS_PER_PGD) * sizeof(pgd_t));
	return pgd;
}

void pgd_free(struct mm_struct *mm, pgd_t *pgd)
{
	kfree(pgd);
}
