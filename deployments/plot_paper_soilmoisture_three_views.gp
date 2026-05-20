# =============================================================================
# plot_paper_soilmoisture_three_views.gp
#
# Two-window comparison of three time series for Soil moisture in the
# Bioretention twin experiment:
#
#   - Truth (synthetic observations, every raw point shown)
#   - Assimilation (operational per-cycle model output, stitched from
#     each cycle's Advance stage)
#   - Reanalysis (the model re-run from t=0 with the final calibrated
#     parameters held fixed)
#
# Soil moisture is the calibrated channel where the operational and
# reanalysis tracks visibly differ: it has the lowest steady-state NSE
# (~0.6) and the operational parameters were furthest from final values
# during the early convergence period. The early window captures this
# divergence; the late window confirms the two tracks converge after
# the calibration has stabilised.
#
# Column layout (paired t,v per channel):
#  15,16: Soil Moisture
#
# Inputs:
#   Bioretention_truth/outputs/selected_output.csv
#   Bioretention_assimilation/outputs/selected_output.csv
#   Bioretention_assimilation/outputs/reanalysis_output.csv
#
# Output:
#   paper_soilmoisture_three_views.png
# =============================================================================

set datafile separator ","
set datafile missing "nan"

truth_file      = "Bioretention_truth/outputs/selected_output.csv"
assim_file      = "Bioretention_assimilation/outputs/selected_output.csv"
reanalysis_file = "Bioretention_assimilation/outputs/reanalysis_output.csv"
outfile         = "paper_soilmoisture_three_views.png"

# --- output canvas --------------------------------------------------------
set terminal pngcairo size 1400,650 enhanced font "Helvetica,18"
set output outfile

# --- time conversions ----------------------------------------------------
epoch(d) = (d - 25569.0) * 86400.0
set xdata time
set timefmt "%s"

# --- window bounds -------------------------------------------------------
# Early window picked to capture the largest assim-vs-reanalysis divergence
# in the soil-moisture record (during the initial parameter transient).
# Late window matches the late window of the truth-vs-assim figures.
early_start = 43833.0   # 2020-01-03
early_end   = 43843.0   # 2020-01-13
late_start  = 44047.0   # 2020-08-04
late_end    = 44057.0   # 2020-08-14

# --- truth sub-sampling ---------------------------------------------------
sub_every = 1

# --- style ---------------------------------------------------------------
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

# ----- Left: early window -- legend lives here ---------------------------
set xrange [epoch(early_start):epoch(early_end)]
set xtics 172800   # 2 days
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
set ylabel "Soil moisture" offset 1,0
set yrange [0.30:0.40]
set ytics 0.02
set format y "%g"
set title "Early window: 2020-01-03 -- 2020-01-13" font "Helvetica,18"

set key bottom right inside box opaque samplen 2 spacing 1.2 \
    font "Helvetica,16" width 0

plot \
    assim_file using (epoch($15)):16 with lines \
        lc rgb c_assim lw lw_curve title "Assimilation (operational)", \
    reanalysis_file using (epoch($15)):16 with lines \
        lc rgb c_reanalysis lw lw_curve title "Reanalysis (final parameters)", \
    truth_file every sub_every using (epoch($15)):16 with points \
        lc rgb c_truth pt 7 ps ps_truth title "Truth (synthetic obs.)"

unset key

# ----- Right: late window ------------------------------------------------
set xrange [epoch(late_start):epoch(late_end)]
set xtics 172800
set format x date_format
set xlabel "Date (2020)" offset 0,-0.5
unset ylabel
set yrange [0.30:0.40]
set format y ""
set title "Late window: 2020-08-04 -- 2020-08-14" font "Helvetica,18"

plot \
    assim_file using (epoch($15)):16 with lines \
        lc rgb c_assim lw lw_curve notitle, \
    reanalysis_file using (epoch($15)):16 with lines \
        lc rgb c_reanalysis lw lw_curve notitle, \
    truth_file every sub_every using (epoch($15)):16 with points \
        lc rgb c_truth pt 7 ps ps_truth notitle

unset multiplot
unset output
