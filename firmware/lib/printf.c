// SPDX-License-Identifier: GPL-3.0-only
/* %d %u %x %s %c %p %%, width, 0 and - flags, l ignored; no floats */

#include "fw/lib.h"

struct out {
	char *buf;
	size_t size;
	size_t len;
};

static void put(struct out *o, char c)
{
	if (o->len + 1 < o->size)
		o->buf[o->len] = c;
	o->len++;
}

static void put_num(struct out *o, u32 v, u32 base, bool neg, int width,
		    bool zero, bool left)
{
	char tmp[12];
	int n = 0, pad;

	do {
		u32 d = v % base;

		tmp[n++] = d < 10 ? '0' + d : 'a' + d - 10;
		v /= base;
	} while (v);

	pad = width - n - neg;
	if (neg && zero)
		put(o, '-');
	while (!left && pad-- > 0)
		put(o, zero ? '0' : ' ');
	if (neg && !zero)
		put(o, '-');
	while (n)
		put(o, tmp[--n]);
	while (left && pad-- > 0)
		put(o, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
	struct out o = { buf, size, 0 };

	for (; *fmt; fmt++) {
		bool zero = false, left = false;
		int width = 0;
		const char *s;
		s32 v;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		for (fmt++; *fmt == '0' || *fmt == '-'; fmt++) {
			zero |= *fmt == '0';
			left |= *fmt == '-';
		}
		for (; *fmt >= '0' && *fmt <= '9'; fmt++)
			width = width * 10 + *fmt - '0';
		while (*fmt == 'l')
			fmt++;

		switch (*fmt) {
		case 'd':
			v = va_arg(ap, s32);
			put_num(&o, v < 0 ? -(u32)v : (u32)v, 10, v < 0, width,
				zero, left);
			break;
		case 'u':
			put_num(&o, va_arg(ap, u32), 10, false, width, zero, left);
			break;
		case 'x':
		case 'p':
			put_num(&o, va_arg(ap, u32), 16, false, width, zero, left);
			break;
		case 'c':
			put(&o, (char)va_arg(ap, int));
			break;
		case 's':
			for (s = va_arg(ap, const char *); s && *s; s++)
				put(&o, *s);
			break;
		case '%':
			put(&o, '%');
			break;
		default:
			return -1;
		}
	}
	if (size)
		buf[o.len < size ? o.len : size - 1] = 0;
	return o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(buf, size, fmt, ap);
	va_end(ap);
	return n;
}
