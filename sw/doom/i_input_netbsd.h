// The keyboard shares the wsdisplay descriptor the video backend opens,
// so that backend hands it over rather than opening the device twice.

#ifndef PENUMBRA_I_INPUT_NETBSD_H
#define PENUMBRA_I_INPUT_NETBSD_H

void I_InputInit(int fd);
void I_InputShutdown(void);

#endif
