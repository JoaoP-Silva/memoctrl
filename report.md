<!-- LTeX: language=pt-BR -->

# PAGINADOR DE MEMÓRIA -- RELATÓRIO

1. Termo de compromisso

	Ao entregar este documento preenchido, os membros do grupo afirmam que todo o código desenvolvido para este trabalho é de autoria própria.  Exceto pelo material listado no item 3 deste relatório, os membros do grupo afirmam não ter copiado material da Internet nem ter obtido código de terceiros.

2. Membros do grupo e alocação de esforço

	* João Pedro Fernandes Silva joaofernandes@dcc.ufmg.br 50%
	* Gabriela Tavares Barreto gtavaresbarreto@gmail.com 50%

3. Referências bibliográficas
 Material dado em sala de aula.

4. Detalhes de implementação

1. Estruturas de Dados

-  Tabela de Páginas e Entradas da Tabela:
A tabela de páginas é implementada como uma lista encadeada onde cada nó é uma table_entry. Cada table_entry contém o número da página, o frame na memória, permissões de acesso, bloco de disco associado e um ponteiro para o próximo nó. A lista encadeada foi a estrutura escolhida por permitir a alocação dinâmica das páginas, facilitando o gerenciamento na hora de inserir e remover nós à medida que as páginas são requisitadas pelo programa. O uso de mutex na tabela de páginas garante que operações simultâneas de múltiplos threads sejam seguras, evitando condições de corrida.

- Lista de Processos Paginados e Entradas da Lista de Processos:
Similarmente à tabela de páginas, a lista de processos paginados é implementada como uma lista encadeada, onde cada plist_node armazena o ID do processo, o número de páginas alocadas e uma referência para a lista de páginas desse processo (p_pages). Uma lista encadeada também foi escolhida para essa implementação por representar processos em execução de forma dinâmica, permitindo a adição e remoção eficiente de processos. O uso de mutex garante a consistência dos dados durante operações que modificam a lista de processos.

- Lista de Páginas de um Processo e Nós da Lista de Páginas:
Cada p_pages_node armazena informações sobre uma página específica do processo, incluindo o bloco de disco, um indicador de uso e um ponteiro para a entrada correspondente na tabela de páginas. Esta estrutura facilita o rastreamento das páginas pertencentes a um processo específico e o gerenciamento da memória de forma eficiente.

- Gerenciamento de Frames e Blocos:
Ambas estruturas utilizam arrays de booleanos para rastrear a utilização de frames de memória e blocos de disco. Mutexes são usados para garantir a segurança durante operações de alocação e liberação. Arrays de booleanos são uma escolha eficiente para representar o estado de uso (livre ou ocupado) de um grande número de recursos de forma compacta.

2. Controle de Acesso e Modificação às Páginas

- Controle de Acesso:
   - O acesso às estruturas principais, como a tabela de páginas, lista de processos e arrays de frames e blocos, é protegido por mutexes. Isso impede que múltiplos threads modifiquem as mesmas estruturas simultaneamente, o que poderia resultar em inconsistências e falhas na memória.
   - Durante uma falha de página, o acesso a uma página é gerenciado da seguinte forma: primeiro é feita a verificação se um frame livre se necessário, e caso seja, ele é alocado. Depois são atualizadas as permissões de acesso à página, usando funções como mmu_resident e mmu_chpro.

2. Modificação de Páginas:
   - As operações que modificam as páginas, com alocação de novos frames ou blocos de disco, são encapsuladas em seções críticas protegidas por mutexes. Isso garante que as modificações sejam realizadas de maneira atômica, sem interferência de outros threads.
   - A remoção de páginas, que ocorre na funcao pager_destroy, também é feita com o uso de mutexes, garantindo que as estruturas de quadros, blocos e páginas sejam deslocados corretamente pelo programa.
