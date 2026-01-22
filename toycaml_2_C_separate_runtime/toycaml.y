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
        {
            cout << "#include <stdio.h>\n";
            cout << "#include \"runtime.h\"\n\n";
            cout << "void start() {\n";
        }
        ExpressionStar
        {
            cout << "}\n\n";
            cout << "int main() {\n";
            cout << "\tinit_heap();\n";
            cout << "\tstart();\n";
            cout << "\treturn 0;\n";
            cout << "}\n";
        }

    ExpressionStar:
        Expression
    |   Expression ExpressionStar

    Expression : 
        Var '=' Rhs ';' 
    {
        if(declared.find(string($1)) == declared.end()){
            cout << "\tlong " << $1 << " = " << $3 << ";\n";
            declared.insert(string($1));
        }
        else {
            cout << "\t" << $1 << " = " << $3 << ";\n";
        }
    }
    |   Field '=' Rhs ';' 
    {
        cout << "\t" << $1 << " = " << $3 << ";\n";
    }
    |   PRINTNUM '(' Var ')' ';' 
    {
        cout << "\tprintf(\"%ld\\n\", val2long(" << $3 << "));\n";
    }

    Rhs:
        INTEGER { $$ = concat(cs("long2val("), concat($1, cs(")"))); }
    |   Field   { $$ = $1; }
    |   Var     { $$ = $1; }
    |   Alloc   { $$ = $1; }
    
    Field:
        FIELD '(' Var ',' Var ')' 
    {
        $$ = concat(cs("Field("), concat($3, concat(cs(", val2long("), concat($5, cs("))")))));
    }

    Alloc:
        ALLOC '(' Var ',' Var ')' 
    {
        $$ = concat(cs("(long)caml_alloc(val2long("), concat($3, concat(cs("), val2long("), concat($5, cs("))")))));
    }

    Var:
        IDENTIFIER { $$ = $1; }

%%

void yyerror(string s) {
    printf("// Failed to parse toycaml code.\n");
    exit(1);
}

int main(void) {
    return yyparse();
}

