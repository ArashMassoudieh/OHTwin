# Auto-fixed: 9-observation truth/assimilation/reanalysis plot.
# Uses CSV headers with column("...") so observation order cannot be mixed.
# Assumes all three files share the selected_output/reanalysis header names.
#
# Run from within deployments/:
#     gnuplot plot_truth_vs_assim_9_rows.gp
# For the drift case (uses Bioretention_*_drift folders, writes
# truth_vs_assim_9_rows_drift.png):
#     gnuplot -e "drift=1" plot_truth_vs_assim_9_rows.gp

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

set datafile separator comma
set xdata time
set timefmt "%s"
set format x "%Y-%m-%d"
set grid
set key right top
set tics out
set border lw 1
set style line 1 lc rgb "#1f77b4" lw 1.4 pt 7 ps 0.25
set style line 2 lc rgb "#d62728" lw 1.4 pt 7 ps 0.25
set style line 3 lc rgb "#2ca02c" lw 1.4 pt 7 ps 0.25

set terminal pngcairo size 1500,2100 enhanced font "Helvetica,13"
set output "truth_vs_assim_9_rows" . suffix . ".png"
set multiplot layout 9,1 title "Truth twin    Assimilation twin    Reanalysis" font "Helvetica,17"
set key horizontal outside top center

set title "Evaporation"
set ylabel "m/day"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Evaporation")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Evaporation")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Evaporation")) with linespoints ls 3 title "Reanalysis"

set title "Groundwater Recharge"
set ylabel "m^3/day"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Groundwater Recharge (m3/day)")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Groundwater Recharge (m3/day)")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Groundwater Recharge (m3/day)")) with linespoints ls 3 title "Reanalysis"

set title "Infiltration rate"
set ylabel "m^3/day"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Infiltration rate (m3/day)")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Infiltration rate (m3/day)")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Infiltration rate (m3/day)")) with linespoints ls 3 title "Reanalysis"

set title "Inflow to the pond"
set ylabel "m^3/day"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Inflow to the pond (m3/day)")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Inflow to the pond (m3/day)")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Inflow to the pond (m3/day)")) with linespoints ls 3 title "Reanalysis"

set title "Overflow"
set ylabel "m^3/day"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Overflow (m3/day)")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Overflow (m3/day)")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Overflow (m3/day)")) with linespoints ls 3 title "Reanalysis"

set title "Pond water depth"
set ylabel "m"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Pond water depth (m)")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Pond water depth (m)")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Pond water depth (m)")) with linespoints ls 3 title "Reanalysis"

set title "Precipitation"
set ylabel "mm/day"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Precipitation (mm/day)")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Precipitation (mm/day)")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Precipitation (mm/day)")) with linespoints ls 3 title "Reanalysis"

set title "Soil Moisture"
set ylabel "(-)"
unset xlabel
set format x ""
plot \
     truth_file using (($1-25569)*86400):(column(" Soil Moisture")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Soil Moisture")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Soil Moisture")) with linespoints ls 3 title "Reanalysis"

set title "Underdrain flow"
set ylabel "m^3/day"
set xlabel "Date"
set format x "%Y-%m-%d"
plot \
     truth_file using (($1-25569)*86400):(column(" Underdrain flow (m3/day)")) with linespoints ls 1 title "Truth twin", \
     assim_file using (($1-25569)*86400):(column(" Underdrain flow (m3/day)")) with linespoints ls 2 title "Assimilation twin", \
     reanalysis_file using (($1-25569)*86400):(column(" Underdrain flow (m3/day)")) with linespoints ls 3 title "Reanalysis"

unset multiplot
print "Wrote truth_vs_assim_9_rows" . suffix . ".png"
