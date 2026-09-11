#pragma once
// Инструмент ICMP ping: запрашивает хост/IP через text_input, затем пингует
// непрерывно (A+B=стоп), показывая sent/recv/loss/RTT в реальном времени.
void k85_run_ping(void);