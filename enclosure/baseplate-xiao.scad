// Baseplate for Seeed Studio XIAO ESP32-C3 with two SparkFun Qwiic
// Single Relay modules.
//
// The XIAO has no mounting holes, so it sits in a friction-fit clip
// holder with side walls and top lips. The two relays use standoffs
// as in the Thing Plus variant.
//
// Layout: the XIAO at the top (centered), two relays side by side
// below it.
//
// Print: flat side down, no supports needed, 0.2mm layer height.

include <baseplate-common.scad>;

// -----------------------------------------------------------------------
// Layout
// -----------------------------------------------------------------------

board_gap = 6;

// Relay boards side by side at the bottom
relay_y     = plate_margin;
relay1_x    = plate_margin;
relay2_x    = plate_margin + qr_pcb_w + board_gap;

total_relay_w = qr_pcb_w * 2 + board_gap;

// XIAO above the relays, centered.
// The clip holder is wider than the XIAO PCB (adds walls).
xiao_holder_w = xiao_pcb_w + board_clearance * 2 + wall_thickness * 2;
xiao_holder_l = xiao_pcb_l + board_clearance * 2 + wall_thickness * 2;
xiao_x = plate_margin + (total_relay_w - xiao_holder_w) / 2;
xiao_y = relay_y + qr_pcb_l + board_gap;

// Overall plate dimensions
plate_w = plate_margin * 2 + total_relay_w;
plate_l = xiao_y + xiao_holder_l + plate_margin;

// -----------------------------------------------------------------------
// Baseplate
// -----------------------------------------------------------------------

linear_extrude(plate_thickness)
    rounded_rect(plate_w, plate_l, plate_corner_r);

// Mounting tabs
translate([0, plate_l / 2, 0])
    rotate([0, 0, -90])
        mounting_tab();

translate([plate_w, plate_l / 2, 0])
    rotate([0, 0, 90])
        mounting_tab();

// -----------------------------------------------------------------------
// XIAO clip holder
// -----------------------------------------------------------------------

translate([xiao_x, xiao_y, 0])
    xiao_holder();

// -----------------------------------------------------------------------
// Relay standoffs
// -----------------------------------------------------------------------

translate([relay1_x, relay_y, 0])
    standoffs_at(qr_holes);

translate([relay2_x, relay_y, 0])
    standoffs_at(qr_holes);

// -----------------------------------------------------------------------
// Labels
// -----------------------------------------------------------------------

label_depth = 0.6;

translate([xiao_x + xiao_holder_w/2, xiao_y + xiao_holder_l + 2, plate_thickness - label_depth])
    linear_extrude(label_depth + 0.1)
        text("XIAO", size=3, halign="center", font="Liberation Sans:style=Bold");

translate([relay1_x + qr_pcb_w/2, relay_y - 3, plate_thickness - label_depth])
    linear_extrude(label_depth + 0.1)
        text("OPEN", size=3, halign="center", font="Liberation Sans:style=Bold");

translate([relay2_x + qr_pcb_w/2, relay_y - 3, plate_thickness - label_depth])
    linear_extrude(label_depth + 0.1)
        text("CLOSE", size=3, halign="center", font="Liberation Sans:style=Bold");
