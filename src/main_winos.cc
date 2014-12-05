/*/////////////////////////////////////////////////////////////////////////////
/// @summary Implements the entry point of the application.
///////////////////////////////////////////////////////////////////////////80*/

/*////////////////
//   Includes   //
////////////////*/
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/*/////////////////
//   Constants   //
/////////////////*/

/*///////////////////
//   Local Types   //
///////////////////*/

/*///////////////////////
//   Local Functions   //
///////////////////////*/

/*////////////////////////
//   Public Functions   //
////////////////////////*/
/// @summary Entry point of the application.
/// @param argc The number of command-line arguments.
/// @param argv An array of NULL-terminated strings specifying command-line arguments.
/// @return Either EXIT_SUCCESS or EXIT_FAILURE.
int main(int argc, char **argv)
{
    int exit_code = EXIT_SUCCESS;

    if (argc > 1)
    {
        /* USAGE */
    }

    exit(exit_code);
}

