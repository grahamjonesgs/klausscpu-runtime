/*
 * libdt2.so: loaded with dlopen by dtmain (not linked at start-up).
 * DT_NEEDED libdt1.so; calls back into the PIE (exported with -rdynamic).
 */
#include "dt1.h"

int pie_callback(int);			/* defined in the PIE */
extern int pie_exported_data;		/* likewise */

__thread int dt2_tls = 9;
__thread char dt2_tls_arr[256] = { [0] = 'x', [255] = 'y' };

int dt2_ctor_ran;
int dt2_ctor_cb;			/* what pie_callback gave the ctor */
int dt2_ctor_dt1;			/* what dt1_add gave the ctor */

__attribute__((constructor)) static void dt2_ctor(void)
{
	dt2_ctor_ran++;
	dt2_ctor_cb = pie_callback(10);
	dt2_ctor_dt1 = dt1_add(20, 22);
}

int dt2_fn(int x) { return dt1_add(x, pie_callback(x)); }
int dt2_read_pie_data(void) { return pie_exported_data; }
int dt2_tls_get(void) { return dt2_tls; }
void dt2_tls_set(int v) { dt2_tls = v; }
int *dt2_tls_addr(void) { return &dt2_tls; }
int dt2_tls_arr_ok(void) { return dt2_tls_arr[0] == 'x' && dt2_tls_arr[255] == 'y' && dt2_tls_arr[1] == 0; }
