module top_module (
    input [31:0] a,
    input [31:0] b,
    output [31:0] sum
);

    wire tmp, tmp2;

    add16 u0 (
        .a(a[15:0]),
        .b(b[15:0]),
        .cin(1'b0),
        .sum(sum[15:0]),
        .cout(tmp)
    );

    add16 u1 (
        .a(a[31:16]),
        .b(b[31:16]),
        .cin(tmp),
        .sum(sum[31:16]),
        .cout(tmp2)
    );

endmodule

module add1 ( input a, input b, input cin,
              output sum, output cout );
    wire tmp;
    assign tmp = a ^ b;
    assign sum = tmp ^ cin;
    assign cout = (a & b) | (a & cin) | (b & cin);
endmodule


module add16 ( input [15:0] a, input[15:0] b, input cin,
               output [15:0] sum, output cout );

    wire [15:0] c;

    add1 u0 ( .a(a[0]), .b(b[0]), .cin(cin), .sum(sum[0]), .cout(c[0]) );

    genvar i;
    generate
        for (i = 1; i < 16; i = i + 1) begin : gen_add
            add1 u ( .a(a[i]), .b(b[i]), .cin(c[i-1]), .sum(sum[i]), .cout(c[i]) );
        end
    endgenerate

    assign cout = c[15];

endmodule