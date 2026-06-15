# =============================================================================
# plot_paper_physical_parameters_wetland.gp
# Wetland calibrated physical-parameter trajectories.
# =============================================================================

set datafile separator ","
set datafile missing "nan"

infile  = "Wetland_assimilation/outputs/calibration/parameter_history.csv"
outfile = "paper_physical_parameters_wetland.png"

set terminal pngcairo size 1600,1200 enhanced font "Helvetica,18"
set output outfile

epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"

# Truth/baseline values from Wetland.ohq parameter definitions.
true_runoff = 0.75
true_outlet_alpha = 20000.0
true_pond_alpha_mult = 1.0
true_evap = 1.0
true_soil_ksat = 0.01

set multiplot layout 3,2
set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3
unset key

c_est   = "#1f77b4"
c_truth = "#d62728"
ps = 1.0
lw_line = 2.2
lw_truth = 2.2

# Actual parameter_history.csv columns:
# 1 cycle, 2 timestamp, 3 t_now
# 4 CatchmentRunoffCoeff
# 5 Evap_Coefficient
# 6 Outflow_Std
# 7 PondAlphaMultiplier
# 8 Soil_Hydraulic_Conductivity
# 9 Stage_Std
# 10 WetlandOutletAlpha

set format x ""
unset xlabel

set ylabel "Runoff coefficient (-)" offset 1,0
set yrange [0:1]
set title "(a) Catchment runoff coefficient" font "Helvetica,18"
plot true_runoff with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
     infile using (epoch($3)):4 with linespoints lc rgb c_est lw lw_line pt 7 ps ps notitle

set ylabel "Outlet {/Symbol a} (m^2/day)" offset 1,0
set logscale y
set format y "10^{%T}"
set yrange [*:*]
set title "(b) Wetland outlet alpha" font "Helvetica,18"
plot true_outlet_alpha with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
     infile using (epoch($3)):10 with linespoints lc rgb c_est lw lw_line pt 7 ps ps notitle
unset logscale y
set format y "%g"

set ylabel "Pond {/Symbol a} multiplier (-)" offset 1,0
set yrange [0:*]
set title "(c) Pond alpha multiplier" font "Helvetica,18"
plot true_pond_alpha_mult with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
     infile using (epoch($3)):7 with linespoints lc rgb c_est lw lw_line pt 7 ps ps notitle

set ylabel "Evaporation coefficient (-)" offset 1,0
set yrange [0:*]
set title "(d) Evaporation coefficient" font "Helvetica,18"
plot true_evap with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
     infile using (epoch($3)):5 with linespoints lc rgb c_est lw lw_line pt 7 ps ps notitle

set format x "%Y-%m"
set xtics 7889400 rotate by -30 offset 0,-0.3
set xlabel "Simulated date" offset 0,-0.8
set bmargin 5

set ylabel "Soil K_{sat} (m/day)" offset 1,0
set logscale y
set format y "10^{%T}"
set yrange [*:*]
set title "(e) Soil hydraulic conductivity" font "Helvetica,18"
set key top right inside box opaque samplen 2 spacing 1.2 font "Helvetica,16" width 0
plot true_soil_ksat with lines dt 2 lc rgb c_truth lw lw_truth title "Synthetic truth", \
     infile using (epoch($3)):8 with linespoints lc rgb c_est lw lw_line pt 7 ps ps title "Estimated"
unset logscale y
set format y "%g"

# Empty sixth panel to keep a balanced 3x2 layout.
unset border; unset xtics; unset ytics; unset xlabel; unset ylabel; unset title; unset key
plot [0:1][0:1] NaN notitle

unset multiplot
unset output
