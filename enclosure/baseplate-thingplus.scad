// Baseplate for SparkFun Thing Plus (ESP32 WROOM or ESP32-C5)
// with two SparkFun Qwiic Single Relay modules.
//
// Layout: the Thing Plus board at the top, two relays side by side
// below it, screw terminals facing outward for easy wiring.
//
// Print: flat side down, no supports needed, 0.2mm layer height.

include <baseplate-common.scad>;

// -----------------------------------------------------------------------
// Layout — positions of each board's origin on the baseplate
// -----------------------------------------------------------------------

// Spacing between boards
board_gap = 6;

// Relay boards side by side at the bottom, screw terminals facing down.
relay_y     = plate_margin;
relay1_x    = plate_margin;
relay2_x    = plate_margin + qr_pcb_w + board_gap;

// Thing Plus above the relays, centered horizontally.
total_relay_w = qr_pcb_w * 2 + board_gap;
tp_x = plate_margin + (total_relay_w - tp_pcb_w) / 2;
tp_y = relay_y + qr_pcb_l + board_gap;

// Overall plate dimensions
plate_w = plate_margin * 2 + total_relay_w;
plate_l = tp_y + tp_pcb_l + plate_margin;

// -----------------------------------------------------------------------
// Baseplate
// -----------------------------------------------------------------------

// Plate
linear_extrude(plate_thickness)
    rounded_rect(plate_w, plate_l, plate_corner_r);

// Mounting tabs on left and right edges
translate([0, plate_l / 2, 0])
    rotate([0, 0, -90])
        mounting_tab();

translate([plate_w, plate_l / 2, 0])
    rotate([0, 0, 90])
        mounting_tab();

// -----------------------------------------------------------------------
// Thing Plus standoffs (2 mounting holes near USB end)
// -----------------------------------------------------------------------

translate([tp_x, tp_y, 0])
    standoffs_at([tp_hole1, tp_hole2]);

// -----------------------------------------------------------------------
// Relay 1 standoffs (4 corners)
// -----------------------------------------------------------------------

translate([relay1_x, relay_y, 0])
    standoffs_at(qr_holes);

// -----------------------------------------------------------------------
// Relay 2 standoffs (4 corners)
// -----------------------------------------------------------------------

translate([relay2_x, relay_y, 0])
    standoffs_at(qr_holes);

// -----------------------------------------------------------------------
// Labels (engraved into plate top surface)
// -----------------------------------------------------------------------

label_depth = 0.6;

translate([tp_x + tp_pcb_w/2, tp_y + tp_pcb_l + 2, plate_thickness - label_depth])
    linear_extrude(label_depth + 0.1)
        text("ESP32", size=3, halign="center", font="Liberation Sans:style=Bold");

translate([relay1_x + qr_pcb_w/2, relay_y - 3, plate_thickness - label_depth])
    linear_extrude(label_depth + 0.1)
        text("OPEN", size=3, halign="center", font="Liberation Sans:style=Bold");

translate([relay2_x + qr_pcb_w/2, relay_y - 3, plate_thickness - label_depth])
    linear_extrude(label_depth + 0.1)
        text("CLOSE", size=3, halign="center", font="Liberation Sans:style=Bold");
