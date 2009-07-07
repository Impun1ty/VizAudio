#include "canberra.h"
#include "vizaudio_hook.h"
#include "vizaudio.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void vizaudio_display(ca_proplist *data) {

    char *out;

    out = (char*) ca_proplist_gets_unlocked(data, CA_PROP_EVENT_ID);

    flash_text(out);

}