#pragma once

void test_rng_word(void);
#define rt_spin_lock_init(lock) ((void)0)
#define rt_spin_lock_irqsave(lock) (test_rng_word(), 0L)
#define rt_spin_unlock_irqrestore(lock, level) ((void)(level))
