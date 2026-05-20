# =============================================================================
# plot_paper_pond_three_views.gp
#
# Two-window comparison of three time series for Pond water depth in
# the Bioretention twin experiment:
#
#   - Truth (synthetic observations, every raw point shown)
#   - Assimilation (operational per-cycle model output, stitched from
#     each cycle's Advance stage)
#   - Reanalysis (the model re-run from t=0 with the final calibrated
#     parameters held fixed)
#
# Pond depth is chosen as the showcase channel because it has the
# cleanest fit among the calibrated channels (R^2 ~ 0.99 by end of
# Phase 1) and the most distinctive temporal structure (sparse storm
# pulses against a near-zero baseline).
#
# Two-window layout matches plot_paper_truth_vs_assim.gp: early window
# (still converging) on the left, late window (steady state) on the
# right, sharing y-range.
#
# Column layout (paired t,v per channel):
#  11,12: Pond water depth (m)
#
# Inputs:
#   Bioretention_truth/outputs/selected_output.csv
#   Bioretention_assimilation/outputs/selected_output.csv
#   Bioretention_assimilation/outputs/reanalysis_output.csv
#
# Output:
#   paper_pond_three_views.png
# =============================================================================

set datafile separator ","
set datafile missing "nan"

truth_file      = "Bioretention_truth/outputs/selected_output.csv"
assim_file      = "Bioretention_assimilation/outputs/selected_output.csv"
reanalysis_file = "Bioretention_assimilation/outputs/reanalysis_output.csv"
outfile         = "paper_pond_three_views.png"

# --- output canvas --------------------------------------------------------
set terminal pngcairo size 1400,650 enhanced font "Helvetica,18"
set output outfile

# --- time conversions -----------------------------------------------------
epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"

# --- window bounds --------------------------------------------------------
early_start = 43865.0   # 2020-02-04
early_end   = 43875.0   # 2020-02-14
late_start  = 44047.0   # 2020-08-04
late_end    = 44057.0   # 2020-08-14

# --- truth sub-sampling ---------------------------------------------------
sub_every = 1

# --- style ----------------------------------------------------------------
c_truth      = "#000000"
c_assim      = "#1f77b4"
c_reanalysis = "#d62728"

ps_truth = 0.5
lw_curve = 1.8

date_format = "%b %d"

# --- layout: 1 row x 2 columns -------------------------------------------
set multiplot layout 1,2

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3

# ----- Left: early window -- legend lives here ----------------------------
set xrange [epoch(early_start):epoch(early_end)]
set xtics 172800   # 2 days
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
set ylabel "Pond water depth (m)" offset 1,0
set yrange [0:0.17]
set ytics 0.05
set format y "%g"
set title "Early window: 2020-02-04 -- 2020-02-14" font "Helvetica,18"

set key top left inside box opaque samplen 2 spacing 1.2 \
    font "Helvetica,16" width 0

plot \
    assim_file using (epoch($11)):12 with lines \
        lc rgb c_assim lw lw_curve title "Assimilation (operational)", \
    reanalysis_file using (epoch($11)):12 with lines \
        lc rgb c_reanalysis lw lw_curve title "Reanalysis (final parameters)", \
    truth_file every sub_every using (epoch($11)):12 with points \
        lc rgb c_truth pt 7 ps ps_truth title "Truth (synthetic obs.)"

unset key

# ----- Right: late window -------------------------------------------------
set xrange [epoch(late_start):epoch(late_end)]
set xtics 172800
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
unset ylabel
set yrange [0:0.17]
set format y ""
set title "Late window: 2020-08-04 -- 2020-08-14" font "Helvetica,18"

plot \
    assim_file using (epoch($11)):12 with lines \
        lc rgb c_assim lw lw_curve notitle, \
    reanalysis_file using (epoch($11)):12 with lines \
        lc rgb c_reanalysis lw lw_curve notitle, \
    truth_file every sub_every using (epoch($11)):12 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

unset multiplot
unset output
