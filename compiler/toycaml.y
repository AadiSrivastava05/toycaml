%{
    #include <bits/stdc++.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <string.h>
    #define cs(x) (strdup(((string)x).c_str()))
    using namespace std;
    void yyerror(string);
    int yylex(void);

    
    char* concat(char* str1, char* str2){
        char * str3 = (char *) malloc(1 + strlen(str1)+ strlen(str2) );
        strcpy(str3, str1);
        strcat(str3, str2);
        return str3;
    }

    set<string> declared;

    bool was_alloc = false;
    string alloc_sz = "0";
    string alloc_tag = "0";
   
%}

%union {
    char* val;
}

%token <val> IDENTIFIER INTEGER
%token FIELD ALLOC PRINTNUM
%type <val> Rhs Var Field Alloc 

%%
    goal:
        {cout<<"#include<stdio.h>\n#include<stdlib.h>\nint main(){\n";}ExpressionStar{cout<<"}\n";}

    ExpressionStar:
        Expression
    |   Expression ExpressionStar

    Expression : 
       Var '=' Rhs ';' 
    {
        if(declared.find(string($1))==declared.end()){
            cout<<'\t'<<"long "<<$1<<" = "<<$3<<";\n";
            declared.insert(string($1));
        }
        else cout<<'\t'<<$1<<" = "<<$3<<";\n";

        if(was_alloc){
            cout<<"\t*(long*)"<<$1<<" = ("<<alloc_sz<<"<<10) + ("<<alloc_tag<<");\n";
        }

        was_alloc = false;
    }
    |   Field '=' Rhs ';' 
    {
        cout<<'\t'<<$1<<" = "<<$3<<";\n";
        if(was_alloc){
            cout<<"\t*(long*)"<<$1<<" = ("<<alloc_sz<<"<<10) + ("<<alloc_tag<<");\n";
        }

        was_alloc = false;
    }
    |   PRINTNUM '(' Var ')' ';' {cout<<"\tprintf(\"%ld\\n\","<<concat(cs("("), concat($3, cs(">>1)")))<<");\n";}

    Rhs:
       INTEGER {$$ = concat(cs("(1+("), concat($1, cs("<<1))")));}
    |   Field {$$ = $1;}
    |   Var {$$ = $1;}
    |   Alloc {$$ = $1;}
    
    Field:
       FIELD '(' Var ',' Var ')' {$$ = concat(concat(cs("*((long*)"), $3), concat(concat(cs(" + "),concat(cs("("), concat($5, cs(">>1)")))),cs(")")));}

    Alloc:
       ALLOC '(' Var ',' Var ')' {$$ =  concat(concat(cs("(long*)malloc(("),concat(cs("("), concat($3, cs(">>1)")))), cs("+1)*(sizeof(long)))")); was_alloc = true; alloc_sz = concat(cs("("), concat($3, cs(">>1)"))); alloc_tag = concat(cs("("), concat($5, cs(">>1)")));}

    Var:
        IDENTIFIER {$$ = $1;}


%%

void yyerror(string s) {
    printf("// Failed to parse toycaml code.\n");
    exit(1);
}

int main(void) {
    // yylex();
    return yyparse();
}

