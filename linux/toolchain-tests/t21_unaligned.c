/* packed / unaligned access */
struct __attribute__((packed)) s { char c; long l; int i; };
long f(struct s *p) { return p->l + p->i; }
