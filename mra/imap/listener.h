#pragma once
void listener_init(int port, int port_ssl);
extern int listener_run();
extern int listener_trigger_accept();
extern void listener_stop_accept();
extern void listener_free();
extern int listener_stop();
