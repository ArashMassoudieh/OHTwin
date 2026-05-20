# =============================================================================
# plot_paper_physical_parameters.gp
#
# Plots per-cycle calibrated trajectories of the four physical parameters
# in the Bioretention twin experiment, each with a horizontal dashed line
# at its synthetic-truth value. Four stacked panels share a single time
# axis.
#
# Inputs:
#   Bioretention_assimilation/outputs/calibration/parameter_history.csv
#
# Output:
#   paper_physical_parameters.png
#
# Run from the deployments/ directory:
#   gnuplot plot_paper_physical_parameters.gp
# =============================================================================

set datafile separator ","
set datafile missing "nan"

infile  = "Bioretention_assimilation/outputs/calibration/parameter_history.csv"
outfile = "paper_physical_parameters.png"

# --- output canvas ----------------------------------------------------------
# Tall portrait for four stacked panels.
set terminal pngcairo size 1100,1500 enhanced font "Helvetica,18"
set output outfile

# --- x-axis: simulated calendar date ---------------------------------------
# parameter_history.csv column 3 is t_now in OHQ day-serial (Excel epoch
# 1899-12-30 UTC). Convert to Unix epoch seconds for gnuplot's time axis:
#   unix = (t_now - 25569) * 86400
epoch(d) = (d - 25569.0) * 86400.0

set xdata time
set timefmt "%s"

# --- synthetic-truth values ------------------------------------------------
# These are the values used to generate the synthetic observation series.
true_Ksat_eng = 10.0
true_evap     = 0.8
true_alpha    = 1.0
true_Ksat_nat = 0.01

# --- shared style ----------------------------------------------------------
set multiplot layout 4,1

set grid xtics ytics lc rgb "#cccccc" lw 0.8
set tics nomirror
set border 3
unset key

c_est   = "#1f77b4"   # estimated trajectory
c_truth = "#d62728"   # truth reference line

ps = 1.0
lw_line = 2.2
lw_truth = 2.2

# Columns in parameter_history.csv:
#   1: cycle
#   2: timestamp
#   3: t_now
#   4: EngineeredSoilKsat
#   5: Evap_Coeff
#   6: NativeSoilAlpha
#   7: NativeSoilKsat
#   8: Std_PondWaterDepth
#   9: Std_SoilMoisture
#  10: Std_UnderdrainFlow

# Suppress x-tic labels and xlabel on the upper three panels.
set format x ""
unset xlabel

# =============================================================================
# Panel (a): EngineeredSoilKsat
# =============================================================================
set ylabel "K_{sat,eng} (m/day)" offset 1,0
set yrange [0:*]
set ytics autofreq
set title "(a) Engineered soil saturated conductivity" font "Helvetica,18"

plot \
    true_Ksat_eng with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):4 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# =============================================================================
# Panel (b): Evap_Coeff
# =============================================================================
set ylabel "Evap. coefficient" offset 1,0
set yrange [0:*]
set ytics autofreq
set title "(b) Evaporation coefficient" font "Helvetica,18"

plot \
    true_evap with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):5 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# =============================================================================
# Panel (c): NativeSoilAlpha
# =============================================================================
set ylabel "{/Symbol a}_{nat} (1/m)" offset 1,0
set yrange [0:*]
set ytics autofreq
set title "(c) Native soil van Genuchten {/Symbol a}" font "Helvetica,18"

plot \
    true_alpha with lines dt 2 lc rgb c_truth lw lw_truth notitle, \
    infile using (epoch($3)):6 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps notitle

# =============================================================================
# Panel (d): NativeSoilKsat -- bottom panel, gets x-axis labels
# =============================================================================
set format x "%Y-%m"
set xlabel "Simulated date" offset 0,-0.5
set ylabel "K_{sat,nat} (m/day)" offset 1,0
set yrange [0:*]
set ytics autofreq
set title "(d) Native soil saturated conductivity" font "Helvetica,18"

# Restore the legend on this panel so the truth/estimated key is visible
# once on the whole figure. The K_sat,nat trajectory hugs the bottom of
# the panel near the truth line, so top-right keeps the legend clear of
# both the curve and the truth dashes.
set key top right inside box opaque samplen 2 spacing 1.2 \
    font "Helvetica,16" width 0

plot \
    true_Ksat_nat with lines dt 2 lc rgb c_truth lw lw_truth title "Synthetic truth", \
    infile using (epoch($3)):7 with linespoints \
        lc rgb c_est lw lw_line pt 7 ps ps title "Estimated"

unset multiplot
unset output
