#include <stdlib.h>
#include <syslog.h>
#include <unordered_map>
#include <unistd.h>
#include "config_interface.h"

bool dual_tor_sock = false;
char loopback[IF_NAMESIZE] = "Loopback0";

static void usage()
{
    printf("Usage: ./dhcp6relay [-u <loopback interface>] [-t <rate>] [-c <count>]\n");
    printf("\tloopback interface: is the loopback interface for dual tor setup\n");
    printf("\t-t <rate>: stress test packet rate (packets per second)\n");
    printf("\t-c <count>: stress test packet count (default: 0, unlimited)\n");
}

int main(int argc, char *argv[]) {
    int stress_rate = 0;
    int stress_count = 0;
    int opt;
    while ((opt = getopt(argc, argv, "u:t:c:h")) != -1) {
        switch (opt) {
            case 'u':
                if (strlen(optarg) != 0 && strlen(optarg) < IF_NAMESIZE) {
                    std::memset(loopback, 0, IF_NAMESIZE);
                    std::memcpy(loopback, optarg, strlen(optarg));
                } else {
                    syslog(LOG_ERR, "loopback interface name over length %d.\n", IF_NAMESIZE);
                    return 1;
                }
                dual_tor_sock = true;
                break;
            case 't':
                stress_rate = atoi(optarg);
                break;
            case 'c':
                stress_count = atoi(optarg);
                break;
            case 'h':
            default:
                usage();
                return 0;
        }
    }
    try {
        std::unordered_map<std::string, relay_config> vlans;
        initialize_swss(vlans);
        loop_relay(vlans, stress_rate, stress_count);
    }
    catch (std::exception &e)
    {
        syslog(LOG_ERR, "An exception occurred.\n");
        return 1;
    }
    return 0;
}
