// SPDX-License-Identifier: GPL-3.0-only
/* Boot and fatal lines; output of two harts may interleave */

#include "fw/csr.h"
#include "fw/lib.h"
#include "plat/plat.h"

static bool con_on;

void con_enable(bool on)
{
	con_on = on;
}

void con_printf(const char *fmt, ...)
{
	char line[96];
	va_list ap;
	int n, i;

	if (!READ_ONCE(con_on))
		return;

	n = snprintf(line, sizeof(line), "[npu%u] ", hart_id());
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof(line) - n, fmt, ap);
	va_end(ap);

	for (i = 0; line[i]; i++) {
		if (line[i] == '\n')
			plat_putc('\r');
		plat_putc(line[i]);
	}
}
