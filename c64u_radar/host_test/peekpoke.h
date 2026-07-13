/* host mock: POKE/PEEK land in the harness's fake 64K RAM */
#ifndef HOST_PEEKPOKE_H
#define HOST_PEEKPOKE_H
void host_poke(unsigned addr, unsigned char v);
unsigned char host_peek(unsigned addr);
#define POKE(a, v) host_poke((unsigned)(a), (unsigned char)(v))
#define PEEK(a)    host_peek((unsigned)(a))
#endif
