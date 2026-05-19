/* easylogging++ requires INITIALIZE_EASYLOGGINGPP in exactly one translation
   unit (it defines el::base::elStorage). electrum-words.cpp pulls epee's
   misc_log_ex.h transitively; this file satisfies the link. */
#include "easylogging++.h"
INITIALIZE_EASYLOGGINGPP
