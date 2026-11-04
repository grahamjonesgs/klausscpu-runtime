/* console_poll.c — polled console input for MicroPython's Zephyr port.
 *
 * The port reads the console through zephyr_getchar*() (its
 * src/zephyr_getchar.c), fed by uart_console's interrupt-driven input hook.
 * The KlaussCPU UART has no RX interrupt, so the link wraps those three
 * functions (-Wl,--wrap, micropython/CMakeLists.txt) with these: a thread
 * polls the UART every tick into a ring buffer and, like the original hook,
 * turns the interrupt character (Ctrl-C) into a KeyboardInterrupt.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

extern int mp_interrupt_char;
void mp_sched_keyboard_interrupt(void);
void mp_hal_signal_event(void);

#define RX_BUFSIZE 1024u	/* power of two */
#define POLL_STACK 1024

static volatile uint8_t rx_buf[RX_BUFSIZE];
static volatile uint32_t rx_get, rx_put;

static const struct device *const console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static K_THREAD_STACK_DEFINE(poll_stack, POLL_STACK);
static struct k_thread poll_thread;

static void rx_poll(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	for (;;) {
		unsigned char ch;
		bool got = false;

		while (uart_poll_in(console, &ch) == 0) {
			if ((int)ch == mp_interrupt_char) {
				mp_hal_signal_event();
				mp_sched_keyboard_interrupt();
				continue;
			}
			uint32_t next = (rx_put + 1u) & (RX_BUFSIZE - 1u);

			if (next == rx_get) {
				continue;	/* full: drop */
			}
			rx_buf[rx_put] = ch;
			rx_put = next;
			got = true;
		}
		if (got) {
			mp_hal_signal_event();
		}
		k_sleep(K_TICKS(1));
	}
}

void __wrap_zephyr_getchar_init(void)
{
	/* Cooperative, so it runs as soon as its tick sleep ends, even while
	 * the (preemptible) MicroPython thread is busy. */
	k_thread_create(&poll_thread, poll_stack, K_THREAD_STACK_SIZEOF(poll_stack),
			rx_poll, NULL, NULL, NULL, K_PRIO_COOP(CONFIG_NUM_COOP_PRIORITIES - 1),
			0, K_NO_WAIT);
	k_thread_name_set(&poll_thread, "mp_rx");
}

int __wrap_zephyr_getchar_check(void)
{
	return rx_get != rx_put;
}

int __wrap_zephyr_getchar(void)
{
	if (rx_get == rx_put) {
		return -1;
	}
	int c = rx_buf[rx_get];

	rx_get = (rx_get + 1u) & (RX_BUFSIZE - 1u);
	return c;
}
