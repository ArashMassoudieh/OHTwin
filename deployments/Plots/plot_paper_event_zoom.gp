#!/usr/bin/env gnuplot
#
# plot_paper_event_zoom.gp
# Event-scale OpenHydroTwin comparison figure.
# Style follows plot_truth_vs_assim.gp.
#
# Run from within deployments/:
#     gnuplot plot_paper_event_zoom.gp
# For the drift case (uses Bioretention_*_drift folders, writes
# paper_event_zoom_drift.png):
#     gnuplot -e "drift=1" plot_paper_event_zoom.gp
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

# Current selected_output.csv layout:
#   1:t  2:Precipitation (mm/day)
#   3:t  4:Underdrain flow (m3/day)
#   5:t  6:Inflow to the pond (m3/day)
#   7:t  8:Groundwater Recharge (m3/day)
#   9:t 10:Overflow (m3/day)
#  11:t 12:Evaporation
#  13:t 14:Infiltration rate (m3/day)
#  15:t 16:Pond water depth (m)
#  17:t 18:Soil Moisture
# OHQ/Excel serial day -> Unix seconds: (serial - 25569)*86400

# Edit this storm window as needed.
x0_serial = 43855
x1_serial = 43862
x0 = (x0_serial - 25569) * 86400
x1 = (x1_serial - 25569) * 86400

set terminal pngcairo size 1800,1600 enhanced font "Helvetica,22"
set output "paper_event_zoom" . suffix . ".png"

set multiplot

set grid lc rgb "#cccccc" lw 0.8
set border lw 1.5
set style line 1 lc rgb "#1f77b4" lw 2.0 pt 7 ps 0.35   # Truth
set style line 2 lc rgb "#d62728" lw 2.2 pt 7 ps 0.35   # Forward twin
set style line 3 lc rgb "#2ca02c" lw 2.2 dt (10,4)       # Reanalysis
set style line 4 lc rgb "#444444" lw 1.7                 # Precipitation

n_panels = 4
top_y    = 0.91
bottom_y = 0.10
panel_h  = (top_y - bottom_y) / n_panels
panel_w  = 0.84
panel_x  = 0.13
panel_y(i) = top_y - i*panel_h

# Legend strip
set origin panel_x, top_y + 0.012
set size   panel_w, 0.055
unset xtics; unset ytics; unset border; unset xlabel; unset ylabel; unset title
set autoscale
set key horizontal center top font "Helvetica,22" samplen 4 spacing 1.2
plot [0:1][0:1] NaN with linespoints ls 1 title "Truth twin", \
                NaN with linespoints ls 2 title "Forward twin", \
                NaN with lines       ls 3 title "Reanalysis"

# Common panel setup
set border lw 1.5
set xtics; set ytics
set tics font "Helvetica,18" nomirror
unset key
set xdata time
set timefmt "%s"
set xrange [x0:x1]
set format x ""
unset xlabel
set ylabel font "Helvetica,23" offset -1,0

# (a) precipitation
set origin panel_x, panel_y(1)
set size   panel_w, panel_h
set ylabel "Precipitation\n(mm/day)"
set label 1 "(a)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [0:*]
plot truth_file using (($13-25569)*86400):14 with impulses ls 4 notitle

# (b) soil moisture, event-scale range
set origin panel_x, panel_y(2)
set size   panel_w, panel_h
set ylabel "Soil\nmoisture (-)"
set label 1 "(b)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [0.31:0.38]
plot truth_file      using (($15-25569)*86400):16 with linespoints ls 1 notitle, \
     assim_file      using (($15-25569)*86400):16 with linespoints ls 2 notitle, \
     reanalysis_file using (($15-25569)*86400):16 with lines       ls 3 notitle

# (c) underdrain flow
set origin panel_x, panel_y(3)
set size   panel_w, panel_h
set ylabel "Underdrain\nflow (m^3/day)"
set label 1 "(c)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [0:*]
plot truth_file      using (($17-25569)*86400):18 with linespoints ls 1 notitle, \
     assim_file      using (($17-25569)*86400):18 with linespoints ls 2 notitle, \
     reanalysis_file using (($17-25569)*86400):18 with lines       ls 3 notitle

# (d) pond water depth
set origin panel_x, panel_y(4)
set size   panel_w, panel_h
set ylabel "Pond water\ndepth (m)"
set label 1 "(d)" at graph 0.015,0.84 font "Helvetica,22" front
set format x "%Y-%m-%d"
set xtics rotate by -30 offset 0,-0.3 font "Helvetica,18"
set xlabel "Date" font "Helvetica,23" offset 0,-1.5
set yrange [0:0.01]
set format y "%.3f"
plot truth_file      using (($11-25569)*86400):12 with linespoints ls 1 notitle, \
     assim_file      using (($11-25569)*86400):12 with linespoints ls 2 notitle, \
     reanalysis_file using (($11-25569)*86400):12 with lines       ls 3 notitle

unset multiplot
unset output
print "Wrote paper_event_zoom" . suffix . ".png"
