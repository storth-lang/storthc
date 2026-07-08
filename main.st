#import "Io"
#import "String"
#import "Json"

struct Person
{
    name: string,
    age: i32,
    score: f64,
    admin: bool,
    langs: [..]any,
}

fn main() -> i32 {
    var := 444 
    xs := type_of(var)
    println("{}", xs)

    src := "{ \"name\": \"Ada Lovelace\", \"age\": 36, \"score\": 99.5, \"admin\": true, \"langs\": [ \"Ada\", \"Urmom\", 30, true, null] }"
    root, ok := json_parse(src);
    if !ok {
        json_report();
        return 1
    }
    p : Person;
    println("{}", p)
    p = json_into(p, root)
    println("{}", p)
    // dump_node(root)
    return 0
}
