#!/usr/bin/env gnuplot
#
# plot_paper_parameter_history.gp
#
# Paper figure: six parameter-trajectory panels (3 rows x 2 columns) showing
# the trajectory of each calibrated parameter across assimilation cycles,
# with a horizontal reference line at the truth value (twin-experiment
# validation).
#
#   (a) EngineeredSoilAlpha  (truth: 1.0   1/m)
#   (b) EngineeredSoilKsat   (truth: 10.0  m/day)
#   (c) EngineeredSoiln      (truth: 1.41)
#   (d) NativeSoilKsat       (truth: 0.01  m/day)
#   (e) NativeSoiln          (truth: 1.41)
#   (f) RunoffCoeff          (truth: 0.8)
#
# Panel boxes are placed with `set lmargin/rmargin/tmargin/bmargin at
# screen` (the gnuplot-recommended way for tiled plots) so all panels are
# *exactly* the same size, regardless of which carries the date labels.
#
# Run from within the deployment folder:
#     gnuplot plot_paper_parameter_history.gp
# For the drift case (uses Bioretention_assimilation_drift, writes
# paper_parameter_history_drift.png):
#     gnuplot -e "drift=1" plot_paper_parameter_history.gp
#
# Output: paper_parameter_history[_drift].png

if (!exists("drift")) drift = 0
if (drift) {
    assim_dir = "Bioretention_assimilation_drift"
    suffix    = "_drift"
} else {
    assim_dir = "Bioretention_assimilation"
    suffix    = ""
}

# ---------- file path ----------
param_file = assim_dir . "/outputs/calibration/parameter_history.csv"

# ---------- CSV columns ----------
#   1 cycle  2 timestamp  3 t_now
#   4 EngineeredSoilAlpha  5 EngineeredSoilKsat  6 EngineeredSoiln
#   7 NativeSoilKsat       8 NativeSoiln         9 RunoffCoeff

# ---------- truth values (edit to match your synthetic-truth setup) ----------
truth_EngAlph  = 1.0
truth_EngK     = 10.0
truth_Engn     = 1.41
truth_NSKsat   = 0.01
truth_NSn      = 1.41
truth_Runoff   = 0.8

set datafile separator ","
set datafile commentschars "#c"   # skip header (starts with 'c' for "cycle")

# ---------- canvas ----------
set terminal pngcairo size 2000,1600 enhanced font "Helvetica,22"
set output "paper_parameter_history" . suffix . ".png"

set multiplot

# ---------- styling ----------
set grid lc rgb "#cccccc" lw 0.8
set border lw 1.5

set style line 1 lc rgb "#d62728" lw 2.6 dt solid pt 7 ps 1.0
set style line 2 lc rgb "#666666" lw 1.5 dt (8,4)

# ---------- screen-coordinate layout (3 rows x 2 cols) ----------
left_x   = 0.085
right_x  = 0.985

# Two columns
col_gap   = 0.07
col_w     = (right_x - left_x - col_gap) / 2.0
xL_left   = left_x
xL_right  = left_x + col_w
xR_left   = xL_right + col_gap
xR_right  = right_x

# Three rows; reserve space at the bottom for date labels + "Date" xlabel.
panel_top_y    = 0.965
panel_bottom_y = 0.115
row_gap        = 0.0
n_rows         = 3
total_height   = panel_top_y - panel_bottom_y - (n_rows - 1) * row_gap
panel_h        = total_height / n_rows

top_r1 = panel_top_y
bot_r1 = top_r1 - panel_h
top_r2 = bot_r1 - row_gap
bot_r2 = top_r2 - panel_h
top_r3 = bot_r2 - row_gap
bot_r3 = top_r3 - panel_h

# ---------- common panel setup ----------
set tics font "Helvetica,18" nomirror
unset key
set autoscale
set xdata time
set timefmt "%s"
set ylabel font "Helvetica,24" offset -1.5,0

# x-axis time expression (Excel serial in t_now -> Unix seconds)
xexpr(c) = (column(3) - 25569) * 86400

# Macro-free helper: panels share most settings; we set per-panel below.

# =====================================================================
# Row 1
# =====================================================================
set format x ""
unset xlabel

# ---------- Panel (a): EngineeredSoilAlpha ----------
set lmargin at screen xL_left
set rmargin at screen xL_right
set tmargin at screen top_r1
set bmargin at screen bot_r1
unset logscale y
set format y "%g"
set yrange [0:*]
set ylabel "Engineered\nSoil α [1/m]"
set label 1 "(a)" at graph 0.03, 0.86 font "Helvetica,22" front
plot truth_EngAlph with lines ls 2 notitle, \
     param_file using ((column(3)-25569)*86400):4 with linespoints ls 1 notitle

# ---------- Panel (b): EngineeredSoilKsat ----------
set lmargin at screen xR_left
set rmargin at screen xR_right
set tmargin at screen top_r1
set bmargin at screen bot_r1
unset logscale y
set format y "%g"
set yrange [0:*]
set ylabel "Engineered\nSoil K_{sat} [m/day]"
set label 1 "(b)" at graph 0.03, 0.86 font "Helvetica,22" front
plot truth_EngK with lines ls 2 notitle, \
     param_file using ((column(3)-25569)*86400):5 with linespoints ls 1 notitle

# =====================================================================
# Row 2
# =====================================================================

# ---------- Panel (c): EngineeredSoiln ----------
set lmargin at screen xL_left
set rmargin at screen xL_right
set tmargin at screen top_r2
set bmargin at screen bot_r2
unset logscale y
set format y "%g"
set yrange [*:*]
set ylabel "Engineered\nSoil n [-]"
set label 1 "(c)" at graph 0.03, 0.86 font "Helvetica,22" front
plot truth_Engn with lines ls 2 notitle, \
     param_file using ((column(3)-25569)*86400):6 with linespoints ls 1 notitle

# ---------- Panel (d): NativeSoilKsat (log scale) ----------
set lmargin at screen xR_left
set rmargin at screen xR_right
set tmargin at screen top_r2
set bmargin at screen bot_r2
set ylabel "Native\nSoil K_{sat} [m/day]"
set label 1 "(d)" at graph 0.03, 0.86 font "Helvetica,22" front
set logscale y
set yrange [0.008:0.2]
set format y "10^{%L}"
plot truth_NSKsat with lines ls 2 notitle, \
     param_file using ((column(3)-25569)*86400):7 with linespoints ls 1 notitle

# =====================================================================
# Row 3 (bottom row carries the date axis)
# =====================================================================
set format x "%Y-%m-%d"
set xtics rotate by -30 offset 0,-0.3 font "Helvetica,18"
set xlabel "Date" font "Helvetica,24" offset 0,-1.5

# ---------- Panel (e): NativeSoiln ----------
set lmargin at screen xL_left
set rmargin at screen xL_right
set tmargin at screen top_r3
set bmargin at screen bot_r3
unset logscale y
set format y "%g"
set yrange [*:*]
set ylabel "Native\nSoil n [-]"
set label 1 "(e)" at graph 0.03, 0.86 font "Helvetica,22" front
plot truth_NSn with lines ls 2 notitle, \
     param_file using ((column(3)-25569)*86400):8 with linespoints ls 1 notitle

# ---------- Panel (f): RunoffCoeff ----------
set lmargin at screen xR_left
set rmargin at screen xR_right
set tmargin at screen top_r3
set bmargin at screen bot_r3
unset logscale y
set format y "%g"
set yrange [0:1]
set ylabel "Runoff\nCoefficient [-]"
set label 1 "(f)" at graph 0.03, 0.86 font "Helvetica,22" front
plot truth_Runoff with lines ls 2 notitle, \
     param_file using ((column(3)-25569)*86400):9 with linespoints ls 1 notitle

unset multiplot
unset output

print "Wrote paper_parameter_history" . suffix . ".png"
