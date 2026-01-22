%{
    #include <bits/stdc++.h>
    #include "toycaml.tab.h"
    using namespace std;
    void yyerror(string);
%}


%%

"caml_alloc" {/*cout<<yytext<<endl; */ return ALLOC;}
"Field" {/* cout<<yytext<<endl; */ return FIELD;}
"print_num" {/* cout<<yytext<<endl; */ return PRINTNUM;}

"="   {/* cout<<yytext<<endl; */ return '=';}

"(" {/* cout<<yytext<<endl; */ return '(';}
")" {/* cout<<yytext<<endl; */ return ')';}

"," {/* cout<<yytext<<endl; */ return ',';}

";" {/* cout<<yytext<<endl; */ return ';';}

[a-zA-Z_][a-zA-Z0-9_]* {
    yylval.val = strdup(yytext);
    /* cout<<yytext<<endl; */ return IDENTIFIER;
}

(0|[1-9][0-9]*) {
    yylval.val = strdup(yytext);
    /* cout<<yytext<<endl; */ return INTEGER;
}

"//".* {
    // yylval.val = strdup(yytext);
    // /* cout<<yytext<<endl; */ return COMMENT;
 }
[ \t\r\n\f][ \t\r\n\f]*  { }
. {yyerror("// Failed to parse toycaml code.\n");}
    
%%
    
    
int yywrap( void ){
    return 1;
}
