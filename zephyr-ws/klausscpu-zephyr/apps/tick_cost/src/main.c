/*
 * tick_cost — what one timer tick costs the interrupted thread.
 *
 * The main thread reads PERF_CYCLES (sys_clock_cycle_get_32) in a tight loop;
 * a gap of more than 100 cycles between two reads is a tick (interrupt entry,
 * register save, sys_clock_isr(), restore, IRET).  With no other thread and
 * no timeout armed, the tick does no scheduling work, so the median is the
 * fixed cost of the interrupt path.  2000 ticks at 1 kHz: 2 s.
 *
 * History (board, 3508f87 bitstream): 657 cycles with the private ISR stack
 * and no DI in the stub; 652 on z_interrupt_stacks with DI.  The harness
 * (zephyr/expect-board.txt) pins the median to 6xx so a slower interrupt path
 * is caught.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/timer/system_timer.h>

#define N 2000
static uint32_t gaps[N];

int main(void)
{
	uint32_t prev = sys_clock_cycle_get_32();
	uint32_t t0 = prev;
	uint64_t spins = 0;
	int n = 0;

	while (n < N) {
		uint32_t now = sys_clock_cycle_get_32();
		uint32_t d = now - prev;

		prev = now;
		spins++;
		if (d > 100) {
			gaps[n++] = d;
		}
	}
	uint32_t total = prev - t0;

	for (int i = 1; i < N; i++) {
		uint32_t v = gaps[i];
		int j = i - 1;

		while (j >= 0 && gaps[j] > v) {
			gaps[j + 1] = gaps[j];
			j--;
		}
		gaps[j + 1] = v;
	}
	printk("TICKCOST min=%u p10=%u median=%u p90=%u max=%u spins=%u cycles=%u\n",
	       gaps[0], gaps[N / 10], gaps[N / 2], gaps[9 * N / 10], gaps[N - 1],
	       (uint32_t)spins, total);
	return 0;
}
