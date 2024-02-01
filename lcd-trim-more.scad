cutoff=1.6;
difference() {
  intersection() {
    import("LCD_fixed.stl", convexity=5);
    translate([0,0,50 + cutoff]) cube([1000,1000,100], center=true);
  }
  difference() {
   translate([0,25,0]) cube([73,50,30], center=true);
   for (i=[-1,1]) {
     translate([i*75/2,15.5,0]) cylinder(d=5, h=30, $fn=20, center=true);
   }
  }
  for (i=[-1,1]) {
  for (j=[-1,1]) {
    translate([i*75/2,j*15.5,0]) cylinder(d=2, h=30, $fn=20, center=true);
  }
  }
}
