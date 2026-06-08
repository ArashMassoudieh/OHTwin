# =============================================================================
# plot_paper_fitness_history_wetland.gp
# Wetland calibration fitness history.
# =============================================================================

set datafile separator ","
set datafile missing "nan"

infile  = "Wetland_assimilation/outputs/calibration/fitness_history_wetland.csv"
outfile = "paper_fitness_history_wetland.png"

set terminal pngcairo size 1100,1400 enhanced font "Helvetica,18"
set output outfile

epoch(d) = (d - 25569.0) * 86400.0
clip(y) = (y < -1.0 ? -1.0 : y)

set xdata time
set timefmt "%s"
set format x "%Y-%m"

c_inlet  = "#1f77b4"
c_mid    = "#2ca02c"
c_outlet = "#d62728"
c_flow   = "#9467bd"

ps = 1.0
lw_line = 2.2

set multiplot layout 3,1
set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

# fitness_history_wetland.csv columns:
# 1 cycle, 2 timestamp, 3 t_now, 4 likelihood
# 5-7 inlet stage MSE/R2/NSE
# 8-10 mid-stage MSE/R2/NSE
# 11-13 outlet stage MSE/R2/NSE
# 14-16 outflow MSE/R2/NSE
# 17 obs/channel, 18 sum_w/channel, 19 sum_w_total, 20 likelihood_per_w

set format x ""
unset xlabel
set key bottom right inside box opaque samplen 2 spacing 1.2 font "Helvetica,16" width 0
set ylabel "NSE" offset 1,0
set yrange [-1.05:1.05]
set ytics 0.5
set title "(a) Nash-Sutcliffe efficiency" font "Helvetica,18"

plot \
    infile using (epoch($3)):(clip($7))  with linespoints lc rgb c_inlet  lw lw_line pt 7 ps ps title "Inlet stage", \
    infile using (epoch($3)):(clip($10)) with linespoints lc rgb c_mid    lw lw_line pt 5 ps ps title "Mid-stage", \
    infile using (epoch($3)):(clip($13)) with linespoints lc rgb c_outlet lw lw_line pt 9 ps ps title "Outlet stage", \
    infile using (epoch($3)):(clip($16)) with linespoints lc rgb c_flow   lw lw_line pt 11 ps ps title "Outflow"

unset key

set ylabel "MSE (channel-native units^2)" offset 1,0
set logscale y
set yrange [*:*]
set format y "10^{%T}"
set ytics autofreq
set title "(b) Mean squared error" font "Helvetica,18"

plot \
    infile using (epoch($3)):5  with linespoints lc rgb c_inlet  lw lw_line pt 7 ps ps notitle, \
    infile using (epoch($3)):8  with linespoints lc rgb c_mid    lw lw_line pt 5 ps ps notitle, \
    infile using (epoch($3)):11 with linespoints lc rgb c_outlet lw lw_line pt 9 ps ps notitle, \
    infile using (epoch($3)):14 with linespoints lc rgb c_flow   lw lw_line pt 11 ps ps notitle

unset logscale y
set format y "%g"

set format x "%Y-%m"
set xtics 7889400
set xlabel "Simulated date" offset 0,-0.5
set bmargin 5
set ylabel "Likelihood / effective N" offset 1,0
set yrange [*:*]
set ytics autofreq
set title "(c) Per-observation negative log-likelihood" font "Helvetica,18"

plot infile using (epoch($3)):($19 > 10 ? $20 : NaN) with linespoints lc rgb "#444444" lw lw_line pt 7 ps ps notitle

unset multiplot
unset output
