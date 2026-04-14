// Shared modules for doorking-esp32 baseplates.
// Dimensions sourced from SparkFun Eagle/KiCad board files and Seeed
// Studio documentation.

// -----------------------------------------------------------------------
// Parameters
// -----------------------------------------------------------------------

// Baseplate
plate_thickness = 3;          // mm
plate_corner_r   = 2;         // corner rounding
plate_margin     = 4;         // extra border around all boards

// Standoffs
standoff_h      = 5;          // height above plate surface
standoff_od     = 6;          // outer diameter
screw_hole_d    = 2.8;        // for M3 or 4-40 self-tapping into plastic
screw_head_sink = 0;          // no countersink by default

// Board clearances
board_clearance = 0.3;        // mm per side for friction-fit holders

// Wall (for XIAO clip holder)
wall_thickness  = 1.6;
clip_lip        = 1.0;        // overhang to hold board down

// Mounting tabs — for attaching the baseplate to a surface
tab_hole_d      = 4.5;        // for #8 or M4 wood screws
tab_width       = 10;
tab_length      = 14;

// -----------------------------------------------------------------------
// SparkFun Thing Plus (Feather form factor)
// Dimensions from Eagle board file for the Thing Plus ESP32 WROOM.
// The Thing Plus C5 uses the same footprint.
// -----------------------------------------------------------------------

tp_pcb_w     = 22.86;         // mm
tp_pcb_l     = 59.69;         // mm
tp_corner_r  = 2.54;          // corner radius on PCB
// Two mounting holes near the bottom edge (USB end)
tp_hole1     = [2.54,  2.54]; // bottom-left
tp_hole2     = [20.32, 2.54]; // bottom-right
tp_hole_d    = 3.048;         // drill diameter

// -----------------------------------------------------------------------
// SparkFun Qwiic Single Relay
// Dimensions from Eagle board file.
// -----------------------------------------------------------------------

qr_pcb_w     = 25.4;          // mm
qr_pcb_l     = 57.15;         // mm
// Four mounting holes, one near each corner
qr_holes     = [
    [2.54,  2.54],             // bottom-left
    [22.86, 2.54],             // bottom-right
    [2.54,  54.61],            // top-left
    [22.86, 54.61],            // top-right
];
qr_hole_d    = 3.302;         // drill diameter
// Screw terminal block at bottom, Qwiic connectors at top
qr_terminal  = [17.78, 10.16];
qr_qwiic1    = [5.08,  48.26];
qr_qwiic2    = [20.32, 48.26];

// -----------------------------------------------------------------------
// Seeed Studio XIAO ESP32-C3
// No mounting holes — uses a friction-fit clip holder.
// -----------------------------------------------------------------------

xiao_pcb_w   = 17.8;          // mm
xiao_pcb_l   = 21.0;          // mm
xiao_pcb_h   = 1.2;           // PCB thickness for clip holder

// -----------------------------------------------------------------------
// Modules
// -----------------------------------------------------------------------

// A single standoff post with a screw hole.
module standoff(h=standoff_h, od=standoff_od, id=screw_hole_d) {
    difference() {
        cylinder(h=h, d=od, $fn=24);
        translate([0, 0, -0.1])
            cylinder(h=h+0.2, d=id, $fn=16);
    }
}

// Place standoffs at a list of [x,y] positions.
module standoffs_at(positions, h=standoff_h) {
    for (p = positions) {
        translate([p[0], p[1], plate_thickness])
            standoff(h);
    }
}

// A rounded rectangle (2D) for plate outlines.
module rounded_rect(w, l, r) {
    offset(r=r) offset(r=-r)
        square([w, l]);
}

// Mounting tab with screw hole, placed at the edge of the plate.
module mounting_tab() {
    difference() {
        translate([-tab_width/2, 0, 0])
            cube([tab_width, tab_length, plate_thickness]);
        translate([0, tab_length/2, -0.1])
            cylinder(h=plate_thickness+0.2, d=tab_hole_d, $fn=16);
    }
}

// XIAO clip holder — friction fit with side walls and top lips.
module xiao_holder(h=standoff_h) {
    inner_w = xiao_pcb_w + board_clearance * 2;
    inner_l = xiao_pcb_l + board_clearance * 2;
    outer_w = inner_w + wall_thickness * 2;
    outer_l = inner_l + wall_thickness * 2;

    translate([0, 0, plate_thickness]) {
        difference() {
            // Outer walls
            linear_extrude(h)
                rounded_rect(outer_w, outer_l, 1);
            // Inner cutout (board pocket)
            translate([wall_thickness, wall_thickness, -0.1])
                cube([inner_w, inner_l, h + 0.2]);
        }

        // Top lips on the long edges to hold the board down.
        // Lips start at PCB thickness height and extend inward.
        for (side = [0, 1]) {
            translate([
                side * (wall_thickness + inner_w) - (side ? clip_lip : 0),
                wall_thickness,
                xiao_pcb_h + 0.5  // slight gap above PCB
            ])
                cube([clip_lip, inner_l, h - xiao_pcb_h - 0.5]);
        }
    }

    // USB-C cutout on one short end
    translate([wall_thickness + inner_w/2 - 5, -0.1, plate_thickness])
        cube([10, wall_thickness + 0.2, h]);
}
