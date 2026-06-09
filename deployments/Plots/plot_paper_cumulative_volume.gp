#!/usr/bin/env gnuplot
#
# plot_paper_cumulative_volume.gp
# Cumulative hydrologic accounting for the bioretention twin.
# Style follows plot_truth_vs_assim.gp.
#
# Run from within deployments/:
#     gnuplot plot_paper_cumulative_volume.gp
# For the drift case (uses Bioretention_*_drift folders, writes
# paper_cumulative_volume_drift.png):
#     gnuplot -e "drift=1" plot_paper_cumulative_volume.gp
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

# Flow outputs are in m^3/day; selected-output save interval is 1 hour.
dt_days = 1.0/24.0

set terminal pngcairo size 1800,1500 enhanced font "Helvetica,22"
set output "paper_cumulative_volume" . suffix . ".png"

set multiplot

set grid lc rgb "#cccccc" lw 0.8
set border lw 1.5
set style line 1 lc rgb "#1f77b4" lw 2.0
set style line 2 lc rgb "#d62728" lw 2.2
set style line 3 lc rgb "#2ca02c" lw 2.2 dt (10,4)

n_panels = 3
top_y    = 0.91
bottom_y = 0.13
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
plot [0:1][0:1] NaN with lines ls 1 title "Truth twin", \
                NaN with lines ls 2 title "Forward twin", \
                NaN with lines ls 3 title "Reanalysis"

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
set yrange [0:*]

# (a) cumulative inflow volume
set origin panel_x, panel_y(1)
set size panel_w, panel_h
set ylabel "Cumulative\ninflow\nvolume (m^3)"
set label 1 "(a)" at graph 0.015,0.84 font "Helvetica,22" front
plot truth_file      using (($7-25569)*86400):($8*dt_days) smooth cumulative with lines ls 1 notitle, \
     assim_file      using (($7-25569)*86400):($8*dt_days) smooth cumulative with lines ls 2 notitle, \
     reanalysis_file using (($7-25569)*86400):($8*dt_days) smooth cumulative with lines ls 3 notitle

# (b) cumulative infiltration volume
set origin panel_x, panel_y(2)
set size panel_w, panel_h
set ylabel "Cumulative\ninfiltration\nvolume (m^3)"
set label 1 "(b)" at graph 0.015,0.84 font "Helvetica,22" front
plot truth_file      using (($5-25569)*86400):($6*dt_days) smooth cumulative with lines ls 1 notitle, \
     assim_file      using (($5-25569)*86400):($6*dt_days) smooth cumulative with lines ls 2 notitle, \
     reanalysis_file using (($5-25569)*86400):($6*dt_days) smooth cumulative with lines ls 3 notitle

# (c) cumulative underdrain volume
set origin panel_x, panel_y(3)
set size panel_w, panel_h
set ylabel "Cumulative\nunderdrain\nvolume (m^3)"
set label 1 "(c)" at graph 0.015,0.84 font "Helvetica,22" front
set format x "%Y-%m-%d"
set xtics rotate by -30 offset 0,-0.3 font "Helvetica,18"
set xlabel "Date" font "Helvetica,23" offset 0,-1.5
plot truth_file      using (($17-25569)*86400):($18*dt_days) smooth cumulative with lines ls 1 notitle, \
     assim_file      using (($17-25569)*86400):($18*dt_days) smooth cumulative with lines ls 2 notitle, \
     reanalysis_file using (($17-25569)*86400):($18*dt_days) smooth cumulative with lines ls 3 notitle

unset multiplot
unset output
print "Wrote paper_cumulative_volume" . suffix . ".png"
