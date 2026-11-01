/* 1 MHz time base from the SoC counter U-Boot's timer uses (clk.c). Any task. */
#ifndef CLK_H
#define CLK_H
unsigned long long clk_us(void);   /* microseconds, monotonic across U-Boot's 4294 s counter resets */
unsigned int clk_ms(void);         /* milliseconds, wraps after 49 days */
void clk_udelay(unsigned int us);  /* busy wait */
#endif
