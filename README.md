SLG
===

SLG is an efficient distributed load injector for Web server. SLG is designed using a master/slave scheme, i.e. a master node  synchronizes a set of load injection nodes (each simulating multiple HTTP clients) and collects their results. SLG is a closed-loop load injector (open-loop mode is experimental).

SLG supports several file access patterns: single file, SPECweb99, SPECweb2005 Support, SPECweb09 Banking Workload.

SLG also has a very experimental support of the memcached protocol, as well as FastCGI protocol (tested for PHP-FPM).

Howto to run:
 * Select the file access pattern is master_slave.h
 * Compile
 * Launch slg on each client node using ./slg <port>
 * Create a configuration file for the master (see conf/ directory for examples)
 * Launch the master using ./slg_master -f <configuration file>

TODO / Known issues
===================

* Use libgsl for statistics


