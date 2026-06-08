#!/usr/bin/env gnuplot
#
# plot_paper_overview_plus_zoom.gp
# Long-record continuity plus event-scale dynamics.
# Style follows plot_truth_vs_assim.gp.
#
# Run from within deployments/:
#     gnuplot plot_paper_overview_plus_zoom.gp
# For the drift case (uses Bioretention_*_drift folders, writes
# paper_overview_plus_zoom_drift.png):
#     gnuplot -e "drift=1" plot_paper_overview_plus_zoom.gp
#

if (!exists("drift")) drift = 0
if (drift) {
    truth_dir = "Bioretention_truth_drift"
    assim_dir = "Bioretention_assimilation_drift"
    suffix    = "_drift"
} else {
    truth_dir = "Bioretention_truth"
    assim_dir = "Bioretention_assimilation"
    suffix    = ""
}

truth_file      = truth_dir . "/outputs/selected_output.csv"
assim_file      = assim_dir . "/outputs/selected_output.csv"
reanalysis_file = assim_dir . "/outputs/reanalysis_output.csv"

set datafile separator ","
set datafile commentschars "#t"

# selected_output.csv layout:
# 1,2 precip; 3,4 underdrain; 5,6 inflow; 7,8 recharge;
# 9,10 overflow; 11,12 evaporation; 13,14 infiltration;
# 15,16 pond depth; 17,18 soil moisture.

# Zoom window, OHQ/Excel serial days.
x0_serial = 43855
x1_serial = 43862
x0 = (x0_serial - 25569) * 86400
x1 = (x1_serial - 25569) * 86400

set terminal pngcairo size 1800,1700 enhanced font "Helvetica,22"
set output "paper_overview_plus_zoom" . suffix . ".png"

set multiplot

set grid lc rgb "#cccccc" lw 0.8
set border lw 1.5
set style line 1 lc rgb "#1f77b4" lw 1.8 pt 7 ps 0.30
set style line 2 lc rgb "#d62728" lw 2.0 pt 7 ps 0.30
set style line 3 lc rgb "#2ca02c" lw 2.0 dt (10,4)
set style line 4 lc rgb "#444444" lw 1.5
set style line 5 lc rgb "#777777" lw 1.2 dt (4,4)

n_panels = 4
top_y    = 0.91
bottom_y = 0.10
panel_h  = (top_y - bottom_y) / n_panels
panel_w  = 0.84
panel_x  = 0.13
panel_y(i) = top_y - i*panel_h

# Legend strip
set origin panel_x, top_y + 0.012
set size panel_w, 0.055
unset xtics; unset ytics; unset border; unset xlabel; unset ylabel; unset title
set autoscale
set key horizontal center top font "Helvetica,22" samplen 4 spacing 1.2
plot [0:1][0:1] NaN with linespoints ls 1 title "Truth twin", \
                NaN with linespoints ls 2 title "Forward twin", \
                NaN with lines       ls 3 title "Reanalysis"

# Common setup
set border lw 1.5
set xtics; set ytics
set tics font "Helvetica,18" nomirror
unset key
set xdata time
set timefmt "%s"
set format x ""
unset xlabel
set ylabel font "Helvetica,23" offset -1,0
set autoscale x
set autoscale y

# (a) full-period underdrain overview.  This panel shows continuity;
# moisture is intentionally excluded because long windows hide its dynamics.
set origin panel_x, panel_y(1)
set size panel_w, panel_h
set ylabel "Underdrain\nflow full\nrecord (m^3/day)"
set label 1 "(a)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [0:*]
set arrow 11 from x0, graph 0 to x0, graph 1 nohead ls 5 front
set arrow 12 from x1, graph 0 to x1, graph 1 nohead ls 5 front
plot truth_file      using (($17-25569)*86400):18 with lines ls 1 notitle, \
     assim_file      using (($17-25569)*86400):18 with lines ls 2 notitle, \
     reanalysis_file using (($17-25569)*86400):18 with lines ls 3 notitle
unset arrow 11
unset arrow 12

# Switch to event window.
set xrange [x0:x1]

# (b) precipitation in zoom window
set origin panel_x, panel_y(2)
set size panel_w, panel_h
set ylabel "Precipitation\n(mm/day)"
set label 1 "(b)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [0:*]
plot truth_file using (($13-25569)*86400):14 with impulses ls 4 notitle

# (c) zoomed soil moisture
set origin panel_x, panel_y(3)
set size panel_w, panel_h
set ylabel "Soil moisture\nzoom (-)"
set label 1 "(c)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [0.31:0.38]
plot truth_file      using (($15-25569)*86400):16 with linespoints ls 1 notitle, \
     assim_file      using (($15-25569)*86400):16 with linespoints ls 2 notitle, \
     reanalysis_file using (($15-25569)*86400):16 with lines       ls 3 notitle

# (d) zoomed underdrain flow
set origin panel_x, panel_y(4)
set size panel_w, panel_h
set ylabel "Underdrain\nflow zoom\n(m^3/day)"
set label 1 "(d)" at graph 0.015,0.84 font "Helvetica,22" front
set format x "%Y-%m-%d"
set xtics rotate by -30 offset 0,-0.3 font "Helvetica,18"
set xlabel "Date" font "Helvetica,23" offset 0,-1.5
set yrange [0:*]
plot truth_file      using (($17-25569)*86400):18 with linespoints ls 1 notitle, \
     assim_file      using (($17-25569)*86400):18 with linespoints ls 2 notitle, \
     reanalysis_file using (($17-25569)*86400):18 with lines       ls 3 notitle

unset multiplot
unset output
print "Wrote paper_overview_plus_zoom" . suffix . ".png"
