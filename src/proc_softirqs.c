#include "common.h"

#define MAX_INTERRUPT_NAME 50

struct cpu_interrupt {
    unsigned long long value;
    RRDDIM *rd;
};

struct interrupt {
    int used;
    char *id;
    char name[MAX_INTERRUPT_NAME + 1];
    RRDDIM *rd;
    unsigned long long total;
    struct cpu_interrupt cpu[];
};

// since each interrupt is variable in size
// we use this to calculate its record size
#define recordsize(cpus) (sizeof(struct interrupt) + (cpus * sizeof(struct cpu_interrupt)))

// given a base, get a pointer to each record
#define irrindex(base, line, cpus) ((struct interrupt *)&((char *)(base))[line * recordsize(cpus)])

static inline struct interrupt *get_interrupts_array(uint32_t lines, int cpus) {
    static struct interrupt *irrs = NULL;
    static uint32_t allocated = 0;

    if(unlikely(lines != allocated)) {
        uint32_t l;
        int c;

        irrs = (struct interrupt *)reallocz(irrs, lines * recordsize(cpus));

        // reset all interrupt RRDDIM pointers as any line could have shifted
        for(l = 0; l < lines ;l++) {
            struct interrupt *irr = irrindex(irrs, l, cpus);
            irr->rd = NULL;
            irr->name[0] = '\0';
            for(c = 0; c < cpus ;c++)
                irr->cpu[c].rd = NULL;
        }

        allocated = lines;
    }

    return irrs;
}

int do_proc_softirqs(int update_every, usec_t dt) {
    (void)dt;
    static procfile *ff = NULL;
    static int cpus = -1, do_per_core = -1;
    struct interrupt *irrs = NULL;

    if(unlikely(do_per_core == -1)) do_per_core = config_get_boolean("plugin:proc:/proc/softirqs", "interrupts per core", 1);

    if(unlikely(!ff)) {
        char filename[FILENAME_MAX + 1];
        snprintfz(filename, FILENAME_MAX, "%s%s", global_host_prefix, "/proc/softirqs");
        ff = procfile_open(config_get("plugin:proc:/proc/softirqs", "filename to monitor", filename), " \t", PROCFILE_FLAG_DEFAULT);
        if(unlikely(!ff)) return 1;
    }

    ff = procfile_readall(ff);
    if(unlikely(!ff)) return 0; // we return 0, so that we will retry to open it next time

    uint32_t lines = procfile_lines(ff), l;
    uint32_t words = procfile_linewords(ff, 0);

    if(unlikely(!lines)) {
        error("Cannot read /proc/softirqs, zero lines reported.");
        return 1;
    }

    // find how many CPUs are there
    if(unlikely(cpus == -1)) {
        uint32_t w;
        cpus = 0;
        for(w = 0; w < words ; w++) {
            if(likely(strncmp(procfile_lineword(ff, 0, w), "CPU", 3) == 0))
                cpus++;
        }
    }

    if(unlikely(!cpus)) {
        error("PLUGIN: PROC_SOFTIRQS: Cannot find the number of CPUs in /proc/softirqs");
        return 1;
    }

    // allocate the size we need;
    irrs = get_interrupts_array(lines, cpus);
    irrs[0].used = 0;

    // loop through all lines
    for(l = 1; l < lines ;l++) {
        struct interrupt *irr = irrindex(irrs, l, cpus);
        irr->used = 0;
        irr->total = 0;

        words = procfile_linewords(ff, l);
        if(unlikely(!words)) continue;

        irr->id = procfile_lineword(ff, l, 0);
        if(unlikely(!irr->id || !irr->id[0])) continue;

        int idlen = strlen(irr->id);
        if(unlikely(irr->id[idlen - 1] == ':'))
            irr->id[idlen - 1] = '\0';

        int c;
        for(c = 0; c < cpus ;c++) {
            if(likely((c + 1) < (int)words))
                irr->cpu[c].value = strtoull(procfile_lineword(ff, l, (uint32_t)(c + 1)), NULL, 10);
            else
                irr->cpu[c].value = 0;

            irr->total += irr->cpu[c].value;
        }

        strncpyz(irr->name, irr->id, MAX_INTERRUPT_NAME);

        irr->used = 1;
    }

    RRDSET *st;

    // --------------------------------------------------------------------

    st = rrdset_find_bytype("system", "softirqs");
    if(unlikely(!st)) st = rrdset_create("system", "softirqs", NULL, "softirqs", NULL, "System softirqs", "softirqs/s", 950, update_every, RRDSET_TYPE_STACKED);
    else rrdset_next(st);

    for(l = 0; l < lines ;l++) {
        struct interrupt *irr = irrindex(irrs, l, cpus);
        if(unlikely(!irr->used)) continue;
        // some interrupt may have changed without changing the total number of lines
        // if the same number of interrupts have been added and removed between two
        // calls of this function.
        if(unlikely(!irr->rd || strncmp(irr->name, irr->rd->name, MAX_INTERRUPT_NAME) != 0)) {
            irr->rd = rrddim_find(st, irr->id);
            if(unlikely(!irr->rd))
                irr->rd = rrddim_add(st, irr->id, irr->name, 1, 1, RRDDIM_INCREMENTAL);
            else
                rrddim_set_name(st, irr->rd, irr->name);

            // also reset per cpu RRDDIMs to avoid repeating strncmp() in the per core loop
            if(likely(do_per_core)) {
                int c;
                for (c = 0; c < cpus ;c++)
                    irr->cpu[c].rd = NULL;
            }
        }
        rrddim_set_by_pointer(st, irr->rd, irr->total);
    }
    rrdset_done(st);

    if(do_per_core) {
        int c;

        for(c = 0; c < cpus ; c++) {
            char id[50+1];
            snprintfz(id, 50, "cpu%d_softirqs", c);

            st = rrdset_find_bytype("cpu", id);
            if(unlikely(!st)) {
                // find if everything is zero
                unsigned long long core_sum = 0 ;
                for(l = 0; l < lines ;l++) {
                    struct interrupt *irr = irrindex(irrs, l, cpus);
                    if(unlikely(!irr->used)) continue;
                    core_sum += irr->cpu[c].value;
                }
                if(unlikely(core_sum == 0)) continue; // try next core

                char title[100+1];
                snprintfz(title, 100, "CPU%d softirqs", c);
                st = rrdset_create("cpu", id, NULL, "softirqs", "cpu.softirqs", title, "softirqs/s", 3000 + c, update_every, RRDSET_TYPE_STACKED);
            }
            else rrdset_next(st);

            for(l = 0; l < lines ;l++) {
                struct interrupt *irr = irrindex(irrs, l, cpus);
                if(unlikely(!irr->used)) continue;
                if(unlikely(!irr->cpu[c].rd)) {
                    irr->cpu[c].rd = rrddim_find(st, irr->id);
                    if(unlikely(!irr->cpu[c].rd))
                        irr->cpu[c].rd = rrddim_add(st, irr->id, irr->name, 1, 1, RRDDIM_INCREMENTAL);
                    else
                        rrddim_set_name(st, irr->cpu[c].rd, irr->name);
                }
                rrddim_set_by_pointer(st, irr->cpu[c].rd, irr->cpu[c].value);
            }
            rrdset_done(st);
        }
    }

    return 0;
}
