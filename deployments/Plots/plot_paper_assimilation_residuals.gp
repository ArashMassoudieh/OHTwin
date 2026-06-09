#!/usr/bin/env gnuplot
#
# plot_paper_assimilation_residuals.gp
# Event-scale residual/correction signals for OpenHydroTwin.
# Style follows plot_truth_vs_assim.gp.
#
# Run from within deployments/:
#     gnuplot plot_paper_assimilation_residuals.gp
# For the drift case (uses Bioretention_*_drift folders, writes
# paper_assimilation_residuals_drift.png):
#     gnuplot -e "drift=1" plot_paper_assimilation_residuals.gp
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

# selected_output.csv has 18 columns; after Unix paste, forward/reanalysis
# columns are shifted by +18 relative to the truth columns.
NSEL = 18

# Column mapping in selected_output.csv.
C_UND_T  = 17; C_UND_Y  = 18
C_POND_T = 11; C_POND_Y = 12
C_SOIL_T = 15; C_SOIL_Y = 16

# Event window. Residual plots are intentionally event-focused so that
# corrections do not disappear over long dry periods.
x0_serial = 43855
x1_serial = 43862
x0 = (x0_serial - 25569) * 86400
x1 = (x1_serial - 25569) * 86400

ta = "< paste -d, " . truth_file . " " . assim_file
tr = "< paste -d, " . truth_file . " " . reanalysis_file

set terminal pngcairo size 1800,1500 enhanced font "Helvetica,22"
set output "paper_assimilation_residuals" . suffix . ".png"

set multiplot

set grid lc rgb "#cccccc" lw 0.8
set border lw 1.5
set style line 1 lc rgb "#d62728" lw 2.2 pt 7 ps 0.35      # Forward - truth
set style line 2 lc rgb "#2ca02c" lw 2.2 dt (10,4)          # Reanalysis - truth
set style line 3 lc rgb "#555555" lw 1.2 dt (4,4)           # zero line

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
plot [0:1][0:1] NaN with linespoints ls 1 title "Forward - truth", \
                NaN with lines       ls 2 title "Reanalysis - truth"

# Common setup
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

# (a) soil moisture residual
set origin panel_x, panel_y(1)
set size panel_w, panel_h
set ylabel "Soil moisture\nresidual (-)"
set label 1 "(a)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [-0.04:0.04]
plot [x0:x1] 0 with lines ls 3 notitle, \
     ta using ((column(C_SOIL_T)-25569)*86400):(column(C_SOIL_Y+NSEL)-column(C_SOIL_Y)) with linespoints ls 1 notitle, \
     tr using ((column(C_SOIL_T)-25569)*86400):(column(C_SOIL_Y+NSEL)-column(C_SOIL_Y)) with lines       ls 2 notitle

# (b) underdrain flow residual
set origin panel_x, panel_y(2)
set size panel_w, panel_h
set ylabel "Underdrain\nresidual\n(m^3/day)"
set label 1 "(b)" at graph 0.015,0.84 font "Helvetica,22" front
set yrange [*:*]
plot [x0:x1] 0 with lines ls 3 notitle, \
     ta using ((column(C_UND_T)-25569)*86400):(column(C_UND_Y+NSEL)-column(C_UND_Y)) with linespoints ls 1 notitle, \
     tr using ((column(C_UND_T)-25569)*86400):(column(C_UND_Y+NSEL)-column(C_UND_Y)) with lines       ls 2 notitle

# (c) pond depth residual
set origin panel_x, panel_y(3)
set size panel_w, panel_h
set ylabel "Pond depth\nresidual (m)"
set label 1 "(c)" at graph 0.015,0.84 font "Helvetica,22" front
set format x "%Y-%m-%d"
set xtics rotate by -30 offset 0,-0.3 font "Helvetica,18"
set xlabel "Date" font "Helvetica,23" offset 0,-1.5
set yrange [-0.01:0.01]
set format y "%.3f"
plot [x0:x1] 0 with lines ls 3 notitle, \
     ta using ((column(C_POND_T)-25569)*86400):(column(C_POND_Y+NSEL)-column(C_POND_Y)) with linespoints ls 1 notitle, \
     tr using ((column(C_POND_T)-25569)*86400):(column(C_POND_Y+NSEL)-column(C_POND_Y)) with lines       ls 2 notitle

unset multiplot
unset output
print "Wrote paper_assimilation_residuals" . suffix . ".png"
