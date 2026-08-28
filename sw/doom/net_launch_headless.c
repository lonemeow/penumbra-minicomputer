// Waiting for the server's launch message, without the textscreen GUI.
//
// The upstream version drives a text-mode dialog while it pumps the
// client and server; the pumping is the part that matters.

#include "config.h"
#include "doomtype.h"
#include "i_system.h"
#include "i_timer.h"
#include "net_client.h"
#include "net_gui.h"
#include "net_server.h"

void
NET_WaitForLaunch(void)
{
    while (net_waiting_for_launch)
    {
        NET_CL_Run();
        NET_SV_Run();

        if (!net_client_connected)
        {
            I_Error("Lost connection to server");
        }

        I_Sleep(100);
    }
}
