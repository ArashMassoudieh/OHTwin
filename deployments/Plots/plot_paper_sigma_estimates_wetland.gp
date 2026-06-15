# =============================================================================
# plot_paper_sigma_estimates_wetland.gp
# Wetland calibrated observation-noise estimates.
# =============================================================================

set datafile separator ","
set datafile missing "nan"

infile  = "Wetland_assimilation/outputs/calibration/parameter_history.csv"
outfile = "paper_sigma_estimates_wetland.png"

set terminal pngcairo size 1100,650 enhanced font "Helvetica,18"
set output outfile

epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"
set format x "%Y-%m"
set xtics 10368000
set xlabel "Simulated date" offset 0,-0.5

set logscale y
set format y "10^{%T}"
set ylabel "Estimated {/Symbol s}_j (channel-native units)" offset 1,0
set yrange [*:*]
set ytics autofreq

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3
set key top right inside box opaque samplen 2 spacing 1.2 font "Helvetica,16" width 0

c_stage = "#1f77b4"
c_flow  = "#d62728"
ps = 1.0
lw_line = 2.2

set title "Estimated channel noise standard deviations" font "Helvetica,18"

# Actual columns: 9 Stage_Std, 6 Outflow_Std
plot \
    infile using (epoch($3)):9 with linespoints lc rgb c_stage lw lw_line pt 7 ps ps title "Stage", \
    infile using (epoch($3)):6 with linespoints lc rgb c_flow  lw lw_line pt 9 ps ps title "Outflow"

unset output
